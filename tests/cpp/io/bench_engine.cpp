// Copyright © Advanced Micro Devices, Inc. All rights reserved.
//
// MIT License
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// bench_engine.cpp
//
// A C++ benchmark for MORI-IO whose measurement methodology exactly matches
// nixlbench (NVIDIA NIXL's xferbench). The point is an apples-to-apples RDMA
// throughput/latency comparison between MORI-IO and NIXL on the same fabric,
// eliminating the gaps that exist between MORI's Python benchmark and nixlbench:
//
//   1. WHOLE-LOOP TIMER (not per-iteration sum).
//      nixlbench wraps ONE timer around the entire num_iter loop
//      (nixl_worker.cpp: total_timer -> total_duration = total_timer.lap()),
//      then throughput = total_bytes / total_duration. MORI's Python bench
//      instead times each iteration (launch+transfer) and SUMS them, which
//      excludes the inter-iteration gap. Here we use a single whole-loop timer.
//
//   2. INLINE SPIN-POLL COMPLETION in the benchmark thread.
//      nixlbench polls agent->getXferStatus() in a tight spin (NIXL_IN_PROG ->
//      continue) directly in the bench thread. We mirror that exactly: the timed
//      loop spins on the transfer status flag (stored by MORI's CQ worker
//      thread). Completion is ALWAYS spin here -- never a cv-block Wait() -- so
//      the measured latency matches nixl (a cv wakeup would add ~5-10us and make
//      numbers non-comparable). MORI's blocking Wait() path is exercised only by
//      the Python benchmark, not this nixl-parity tool.
//
//   3. WARMUP excluded from timing (nixl: --warmup_iter, default 100).
//
//   4. throughput_gb = total_bytes / 1e9 / (total_duration_us / 1e6), GB=10^9,
//      identical unit to nixl. total_bytes = msg * batch * num_iter, and
//      avg_latency = total_duration / (num_iter * batch) -- i.e. PER SINGLE
//      TRANSFER, matching nixl's total_duration/(per_thread_iter*batch_size)
//      (nixl counts block_size*batch_size descriptors per request).
//
// Rendezvous: 2 processes (one per node) exchange EngineDesc and MemoryDesc over
// mori::application::SocketBootstrapNetwork (msgpack, same as MORI's pybind
// pack()/unpack()).
// Rank 0 = initiator, rank 1 = target. Initiator drives all transfers (RDMA
// one-sided WRITE/READ); target only registers memory and waits.
//
// Build: see build.sh (links libmori_io + libmori_application from the editable
// install, includes 3rdparty/msgpack-c + HIP).

#include <hip/hip_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <msgpack.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mori/application/bootstrap/socket_bootstrap.hpp"
#include "mori/io/io.hpp"

using namespace mori::io;
using Clock = std::chrono::steady_clock;

#define HIP_CHECK(expr)                                                                           \
  do {                                                                                            \
    hipError_t _e = (expr);                                                                       \
    if (_e != hipSuccess) {                                                                       \
      std::cerr << "HIP error " << hipGetErrorString(_e) << " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                                     \
      std::exit(1);                                                                               \
    }                                                                                             \
  } while (0)

// ------------------------------ validation ---------------------------------
// The Python benchmark checks correctness as part of the run (benchmark.py
// _validate_rdma): it performs a transfer, ships the target's buffer back over
// the control plane and byte-compares it against the initiator's source. We do
// the same check here, but exchange a per-slot checksum instead of the payload,
// so validating a multi-hundred-MiB sweep point stays O(batch) on the wire while
// still covering every transferred byte.
//
// Both buffers are seeded with a rank-dependent, offset-dependent pattern, so an
// un-issued transfer, a short transfer, or one landing at the wrong offset all
// leave the two sides disagreeing.

static uint64_t Fnv1a(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// Byte at absolute offset p is a function of (p, rank), so the two ranks start
// out differing everywhere and every byte position is distinguishable.
static void FillPattern(void* buf, size_t bytes, int rank) {
  constexpr size_t kTile = 1u << 20;
  std::vector<uint8_t> tile(std::min(bytes, kTile));
  for (size_t base = 0; base < bytes; base += tile.size()) {
    const size_t n = std::min(tile.size(), bytes - base);
    for (size_t i = 0; i < n; ++i) {
      const uint64_t p = base + i;
      tile[i] = static_cast<uint8_t>((p * 2654435761ull + rank * 0x9E3779B9ull) >> 13);
    }
    HIP_CHECK(hipMemcpy(static_cast<char*>(buf) + base, tile.data(), n, hipMemcpyHostToDevice));
  }
}

// One checksum per transferred slot, so a mismatch names the slot that differs
// rather than just failing the whole point.
static std::vector<uint64_t> SlotChecksums(const void* buf, const SizeVec& offsets, size_t msg) {
  std::vector<uint64_t> sums(offsets.size());
  std::vector<uint8_t> host(msg);
  for (size_t i = 0; i < offsets.size(); ++i) {
    HIP_CHECK(hipMemcpy(host.data(), static_cast<const char*>(buf) + offsets[i], msg,
                        hipMemcpyDeviceToHost));
    sums[i] = Fnv1a(host.data(), msg);
  }
  return sums;
}

// Forward declaration; defined after Rendezvous/Pack below.
struct Args;

// ------------------------- control-plane rendezvous -------------------------
// Adapter over MORI's SocketBootstrapNetwork -- the same bootstrap layer
// src/cco/cco_init.cpp and src/shmem/init.cpp use to bring up multi-node jobs --
// so the benchmark inherits the library's rendezvous policy instead of carrying a
// second one: connect and accept retried over the MORI_BOOTSTRAP_TIMEOUT budget
// (300s default), a Barrier that blocks for as long as the initiator's sweep
// takes, failures raised as exceptions, and Finalize() from the destructor.
//
// Both ranks derive the same UniqueId from the master endpoint, so nothing has to
// carry it between them. The bootstrap picks its own local interface; set
// MORI_SOCKET_IFNAME when that choice has no route to the peer.
class Rendezvous {
 public:
  Rendezvous(int rank, const std::string& masterIp, uint16_t port)
      : boot(mori::application::SocketBootstrapNetwork::GenerateUniqueId(masterIp, port), rank,
             kWorldSize),
        myRank(rank) {
    boot.Initialize();
  }

  // Contribute this rank's blob, return the peer's. Allgather is fixed-size while
  // packed descriptors are not, so sizes go first and the payload round pads every
  // rank up to the larger of the two.
  std::string ExchangeBlob(const std::string& mine) {
    uint64_t sizes[kWorldSize] = {};
    uint64_t mySize = mine.size();
    boot.Allgather(&mySize, sizes, sizeof(mySize));

    const size_t stride = std::max(sizes[0], sizes[1]);
    if (stride == 0) return {};

    std::vector<char> sendBuf(stride, 0), recvBuf(stride * kWorldSize, 0);
    std::memcpy(sendBuf.data(), mine.data(), mine.size());
    boot.Allgather(sendBuf.data(), recvBuf.data(), stride);

    const int peer = 1 - myRank;
    return std::string(recvBuf.data() + peer * stride, sizes[peer]);
  }

  void Barrier() { boot.Barrier(); }

 private:
  // Rank 0 initiates, rank 1 targets.
  static constexpr int kWorldSize = 2;

  mori::application::SocketBootstrapNetwork boot;
  const int myRank;
};

template <typename T>
static std::string Pack(const T& v) {
  msgpack::sbuffer buf;
  msgpack::pack(buf, v);
  return std::string(buf.data(), buf.size());
}
template <typename T>
static T Unpack(const std::string& b) {
  auto oh = msgpack::unpack(b.data(), b.size());
  return oh.get().as<T>();
}

// Compare the transferred slots of both ranks' buffers after the sweep. Both
// sides derive the geometry from the same args, so the only thing that crosses
// the wire is one checksum per slot. Only the initiator reports.
//
// BOTH ranks must call this unconditionally, even when --skip-validate is set:
// the exchange is a collective, so a rank that returned early would leave its
// peer blocked in Allgather until the bootstrap timeout. Opting out therefore
// contributes an empty checksum vector rather than skipping the call, which also
// makes the flag safe to pass to only one side -- either empty vector downgrades
// the run to "skipped" instead of hanging or reporting a bogus failure.
static bool ValidateTransfer(Rendezvous& rdv, const void* buf, size_t msg, int batch, bool batched,
                             bool batchContiguous, int rank, bool wanted) {
  std::vector<uint64_t> mine;
  if (wanted) {
    // Mirror the offsets the sweep actually used: the batched path strides by
    // msg+1 unless --batch-contiguous, while the singles path is always
    // contiguous (same asymmetry as the Python bench's run_single_once).
    const size_t stride = batched ? (batchContiguous ? msg : msg + 1) : msg;
    SizeVec offsets(static_cast<size_t>(batch));
    for (int i = 0; i < batch; ++i) offsets[i] = static_cast<size_t>(i) * stride;
    mine = SlotChecksums(buf, offsets, msg);
  }

  const auto peer = Unpack<std::vector<uint64_t>>(rdv.ExchangeBlob(Pack(mine)));
  if (rank != 0) return true;

  if (mine.empty() || peer.empty()) {
    std::cout << "validation: skipped"
              << (mine.empty() ? "" : " (peer opted out with --skip-validate)") << std::endl;
    return true;
  }
  if (peer.size() != mine.size()) {
    std::cerr << "VALIDATION FAILED: peer returned " << peer.size() << " checksums, expected "
              << mine.size() << std::endl;
    return false;
  }
  for (size_t i = 0; i < mine.size(); ++i) {
    if (mine[i] != peer[i]) {
      std::cerr << "VALIDATION FAILED: slot " << i << " of " << mine.size()
                << " differs at msg=" << msg << " batch=" << batch << " (initiator " << std::hex
                << mine[i] << " vs target " << peer[i] << std::dec << ")" << std::endl;
      return false;
    }
  }
  std::cout << "validation: OK (" << batch << " slot(s) x " << msg << " B byte-identical)"
            << std::endl;
  return true;
}

// ------------------------------- config ------------------------------------
// Flags mirror MORI's Python benchmark (tests/python/io/benchmark.py) so runs
// are directly comparable. Names use the Python spelling where they exist.
struct Args {
  int rank = 0;           // 0=initiator, 1=target
  std::string master_ip;  // bootstrap root (rank 0's data IP); both ranks pass the same
  std::string self_ip;    // this rank's own reachable IP (advertised in EngineDesc)
  uint16_t port = 18515;
  int gpu = 0;
  int target_dev_offset = 0;     // --target-dev-offset (target GPU = (gpu+offset)%ndev)
  std::string op = "write";      // write|read
  std::string backend = "rdma";  // --backend rdma|xgmi (xgmi = intra-node GPU<->GPU)

  // Sweep control (Python parity). --all sweeps message size; --all-batch sweeps
  // batch size. Neither set => single run at (buffer_size, batch).
  bool sweep_all = false;       // --all
  bool sweep_batch = false;     // --all-batch
  size_t buffer_size = 32768;   // --buffer-size (single message size when not sweeping)
  size_t sweep_start = 8;       // --sweep-start-size (Python default 8)
  size_t sweep_max = 1u << 20;  // --sweep-max-size (Python default 2^20)
  size_t sweep_step = 0;        // --sweep-step (0 = geometric x2; >0 = linear +step)
  int iters = 500;
  int warmup = 50;  // --warmup-iters

  // RdmaBackendConfig knobs
  int qp_per_transfer = 4;               // --num-qp-per-transfer
  int worker_threads = 1;                // --num-worker-threads
  int post_batch_size = -1;              // --post-batch-size
  std::string poll_cq_mode = "polling";  // polling|event
  bool disable_chunking = false;         // --disable-chunking (default: chunking ON, like Python)
  size_t chunk_bytes = 65536;            // --chunk-bytes
  int max_chunks = 64;                   // --max-chunks
  int max_send_wr = 0;                   // --max-send-wr (0 = leave default)
  int max_cqe_num = 0;                   // --max-cqe-num
  int max_msg_sge = 0;                   // --max-msg-sge

  // XgmiBackendConfig knobs (--backend xgmi)
  int num_streams = 64;  // --num-streams
  int num_events = 64;   // --num-events

  // batch / session
  int batch = 1;                      // --transfer-batch-size (transfers per request)
  bool enable_batch_transfer = true;  // --enable-batch-transfer / --disable-batch-transfer.
                                      // ON (default): batch>1 => ONE N-descriptor batch request
                                      // (nixl-equivalent). OFF: batch>1 => N individual single
                                      // transfers per iteration (Python run_single_once path).
  bool batch_contiguous = false;      // --batch-contiguous (adjacent offsets → merged WR);
                                      // default strided (each transfer a separate WR)
  bool enable_sess = false;           // --enable-sess (session fast-path); Python default: off
  bool prepare_once = false;  // --prepare-once (build WRs once, re-post each iter; nixl-style
                              // createXferReq/postXferReq split). Requires --enable-sess.

  std::string mem_type = "gpu";  // --mem-type gpu|cpu
  std::string init_mem_type;     // --initiator-mem-type (empty => mem_type)
  std::string target_mem_type;   // --target-mem-type   (empty => mem_type)

  std::string log_level = "info";  // --log-level trace|debug|info|warning|error|critical

  // Correctness check on the last sweep point, on by default to match the Python
  // benchmark (which always validates). --skip-validate opts out.
  bool skip_validate = false;  // --skip-validate
};

static Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&]() { return std::string(argv[++i]); };
    if (k == "--rank")
      a.rank = std::stoi(next());
    else if (k == "--master-ip")
      a.master_ip = next();
    else if (k == "--self-ip")
      a.self_ip = next();
    else if (k == "--port")
      a.port = static_cast<uint16_t>(std::stoi(next()));
    else if (k == "--gpu")
      a.gpu = std::stoi(next());
    else if (k == "--target-dev-offset")
      a.target_dev_offset = std::stoi(next());
    else if (k == "--op" || k == "--op-type")
      a.op = next();
    else if (k == "--backend")
      a.backend = next();
    else if (k == "--all")
      a.sweep_all = true;
    else if (k == "--all-batch")
      a.sweep_batch = true;
    else if (k == "--buffer-size")
      a.buffer_size = std::stoull(next());
    else if (k == "--sweep-start" || k == "--sweep-start-size")
      a.sweep_start = std::stoull(next());
    else if (k == "--sweep-max" || k == "--sweep-max-size")
      a.sweep_max = std::stoull(next());
    else if (k == "--sweep-step")
      a.sweep_step = std::stoull(next());
    else if (k == "--iters")
      a.iters = std::stoi(next());
    else if (k == "--warmup" || k == "--warmup-iters")
      a.warmup = std::stoi(next());
    else if (k == "--qp-per-transfer" || k == "--num-qp-per-transfer")
      a.qp_per_transfer = std::stoi(next());
    else if (k == "--worker-threads" || k == "--num-worker-threads")
      a.worker_threads = std::stoi(next());
    else if (k == "--post-batch-size")
      a.post_batch_size = std::stoi(next());
    else if (k == "--poll_cq_mode" || k == "--poll-cq-mode")
      a.poll_cq_mode = next();
    else if (k == "--disable-chunking")
      a.disable_chunking = true;
    else if (k == "--chunk-bytes")
      a.chunk_bytes = std::stoull(next());
    else if (k == "--max-chunks")
      a.max_chunks = std::stoi(next());
    else if (k == "--max-send-wr")
      a.max_send_wr = std::stoi(next());
    else if (k == "--max-cqe-num")
      a.max_cqe_num = std::stoi(next());
    else if (k == "--max-msg-sge")
      a.max_msg_sge = std::stoi(next());
    else if (k == "--num-streams")
      a.num_streams = std::stoi(next());
    else if (k == "--num-events")
      a.num_events = std::stoi(next());
    else if (k == "--batch" || k == "--transfer-batch-size")
      a.batch = std::stoi(next());
    else if (k == "--enable-batch-transfer")
      a.enable_batch_transfer = true;
    else if (k == "--disable-batch-transfer")
      a.enable_batch_transfer = false;
    else if (k == "--batch-contiguous")
      a.batch_contiguous = true;
    else if (k == "--enable-sess")
      a.enable_sess = true;
    else if (k == "--disable-sess")
      a.enable_sess = false;
    else if (k == "--prepare-once")
      a.prepare_once = true;
    else if (k == "--mem-type")
      a.mem_type = next();
    else if (k == "--initiator-mem-type")
      a.init_mem_type = next();
    else if (k == "--target-mem-type")
      a.target_mem_type = next();
    else if (k == "--skip-validate")
      a.skip_validate = true;
    else if (k == "--log-level")
      a.log_level = next();
    else {
      std::cerr << "unknown arg " << k << std::endl;
      std::exit(1);
    }
  }
  return a;
}

// ------------------------------ main ---------------------------------------
static int RunBenchmark(int argc, char** argv) {
  Args a = ParseArgs(argc, argv);
  SetLogLevel(a.log_level);

  if (a.backend != "rdma" && a.backend != "xgmi") {
    std::cerr << "--backend must be rdma or xgmi (got " << a.backend << ")" << std::endl;
    return 1;
  }
  // Only the RDMA backend implements prepared transfers; elsewhere PrepareBatch
  // hands back nothing and the run would quietly fall back to the inline path
  // mid-sweep, reporting a prepared number that never ran prepared.
  if (a.prepare_once && a.backend != "rdma") {
    std::cerr << "--prepare-once requires the rdma backend (got " << a.backend << ")" << std::endl;
    return 1;
  }

  // Per-role memory type: initiator (rank 0) / target (rank 1) may override the
  // shared --mem-type, enabling mixed CPU<->GPU transfers. Each process only
  // allocates its own side, so no cross-node coupling is needed.
  const std::string myMem = (a.rank == 0)
                                ? (!a.init_mem_type.empty() ? a.init_mem_type : a.mem_type)
                                : (!a.target_mem_type.empty() ? a.target_mem_type : a.mem_type);
  const bool cpuMem = (myMem == "cpu");
  const MemoryLocationType memLoc = cpuMem ? MemoryLocationType::CPU : MemoryLocationType::GPU;

  // Target GPU shift for cross-rail pairing (GPU memory only, matches Python).
  int gpu = a.gpu;
  if (a.rank == 1 && !cpuMem && a.target_dev_offset != 0) {
    int ndev = 0;
    HIP_CHECK(hipGetDeviceCount(&ndev));
    if (ndev > 0) gpu = (a.gpu + a.target_dev_offset) % ndev;
  }
  HIP_CHECK(hipSetDevice(gpu));  // valid device context needed even for host mem

  // Build the run plan: a list of (msgSize, batch) points.
  //   --all       => sweep message size (geometric x2, or linear when --sweep-step>0),
  //                  batch fixed at --transfer-batch-size.
  //   --all-batch => msg fixed at --buffer-size, batch = 1,2,4,...,32768.
  //   neither     => single point (--buffer-size, --transfer-batch-size).
  std::vector<std::pair<size_t, int>> plan;
  if (a.sweep_all) {
    for (size_t msg = a.sweep_start; msg <= a.sweep_max;
         msg = (a.sweep_step > 0) ? msg + a.sweep_step : msg * 2) {
      plan.emplace_back(msg, a.batch);
    }
  } else if (a.sweep_batch) {
    for (int b = 1; b <= 32768; b *= 2) plan.emplace_back(a.buffer_size, b);
  } else {
    plan.emplace_back(a.buffer_size, a.batch);
  }

  // Buffer must hold the largest single REQUEST across the whole plan. Strided
  // batch (default) needs (msg+1)*batch to keep slots non-adjacent; contiguous
  // needs msg*batch.
  size_t bufBytes = 0;
  for (auto& planEntry : plan) {
    const size_t msg = planEntry.first;
    const int b = planEntry.second;
    const bool pBatched = b > 1;
    const size_t slotStride = a.batch_contiguous ? msg : (msg + 1);
    const size_t need = pBatched ? slotStride * static_cast<size_t>(b) : msg;
    bufBytes = std::max(bufBytes, need);
  }

  void* buf = nullptr;
  if (cpuMem) {
    HIP_CHECK(hipHostMalloc(&buf, bufBytes, 0));
  } else {
    HIP_CHECK(hipMalloc(&buf, bufBytes));
  }
  // Rank-dependent seed pattern rather than zeros, so the post-sweep validation
  // can tell "the transfer moved my bytes" from "both buffers happened to match".
  if (a.skip_validate) {
    HIP_CHECK(hipMemset(buf, 0, bufBytes));
  } else {
    FillPattern(buf, bufBytes, a.rank);
  }

  // Out-of-band control endpoint for the MORI engine. host MUST be an IP the
  // peer can reach (advertised via EngineDesc for RDMA QP setup). Defaults to
  // master_ip: correct for rank 0 (it IS the master); rank 1 should pass
  // --self-ip when its reachable IP differs from the master's.
  IOEngineConfig cfg;
  cfg.host = !a.self_ip.empty() ? a.self_ip : a.master_ip;
  cfg.port = static_cast<uint16_t>(a.port + 1 + a.rank);  // distinct per rank
  std::string key = a.rank == 0 ? "initiator" : "target";
  IOEngine engine(key, cfg);

  // XGMI moves data over Infinity Fabric between two GPUs on the SAME host, so
  // its knobs are stream/event depth rather than QPs and chunking. The RDMA-only
  // flags above are simply unused on that path.
  if (a.backend == "xgmi") {
    XgmiBackendConfig xgmiCfg{};
    xgmiCfg.numStreams = a.num_streams;
    xgmiCfg.numEvents = a.num_events;
    engine.CreateBackend(BackendType::XGMI, xgmiCfg);
  } else {
    RdmaBackendConfig rdmaCfg{};
    rdmaCfg.qpPerTransfer = a.qp_per_transfer;
    rdmaCfg.postBatchSize = a.post_batch_size;
    rdmaCfg.numWorkerThreads = a.worker_threads;
    rdmaCfg.pollCqMode = (a.poll_cq_mode == "event") ? PollCqMode::EVENT : PollCqMode::POLLING;
    rdmaCfg.enableNotification = false;                    // match MORI Python bench RDMA path
    rdmaCfg.enableTransferChunking = !a.disable_chunking;  // chunking ON by default (Python parity)
    rdmaCfg.chunkBytes = a.chunk_bytes;
    rdmaCfg.maxChunksPerTransfer = a.max_chunks;
    if (a.max_send_wr > 0) rdmaCfg.maxSendWr = a.max_send_wr;
    if (a.max_cqe_num > 0) rdmaCfg.maxCqeNum = a.max_cqe_num;
    if (a.max_msg_sge > 0) rdmaCfg.maxMsgSge = a.max_msg_sge;
    engine.CreateBackend(BackendType::RDMA, rdmaCfg);
  }

  MemoryDesc localMem = engine.RegisterMemory(buf, bufBytes, gpu, memLoc);

  // --- rendezvous over MORI's socket bootstrap ------------------------------
  Rendezvous rdv(a.rank, a.master_ip, a.port);

  // Exchange EngineDesc then register the remote engine.
  EngineDesc myEng = engine.GetEngineDesc();
  EngineDesc peerEng = Unpack<EngineDesc>(rdv.ExchangeBlob(Pack(myEng)));
  engine.RegisterRemoteEngine(peerEng);

  // Exchange MemoryDesc.
  MemoryDesc peerMem = Unpack<MemoryDesc>(rdv.ExchangeBlob(Pack(localMem)));

  rdv.Barrier();

  if (a.rank == 1) {
    // Target: nothing to drive. RDMA one-sided ops complete without target CPU.
    // Just hold memory registered until the initiator says it's done.
    rdv.Barrier();  // wait for initiator to finish the whole sweep
    // Collective with the initiator's call below, so it runs even under
    // --skip-validate; the target only contributes checksums and does not report.
    const auto& last = plan.back();
    ValidateTransfer(rdv, buf, last.first, last.second, a.enable_batch_transfer && last.second > 1,
                     a.batch_contiguous, a.rank, !a.skip_validate);
    // Drop the NIC registration before releasing its backing pages, so the
    // backend never holds an MR over freed storage.
    engine.DeregisterMemory(localMem);
    if (cpuMem)
      HIP_CHECK(hipHostFree(buf));
    else
      HIP_CHECK(hipFree(buf));
    return 0;
  }

  // -------------------- initiator: run the sweep ---------------------------
  // Session fast-path (matches MORI --enable-sess). When --disable-sess, we call
  // the engine batch/single APIs directly with explicit MemoryDesc + uid vecs.
  const bool useSess = a.enable_sess;
  IOEngineSession* sessPtr = nullptr;
  std::optional<IOEngineSession> sessOpt;
  if (useSess) {
    sessOpt = engine.CreateSession(localMem, peerMem);
    if (!sessOpt) {
      std::cerr << "CreateSession failed" << std::endl;
      std::exit(1);
    }
    sessPtr = &*sessOpt;
  }
  const bool isRead = (a.op == "read");

  if (a.prepare_once && !useSess) {
    std::cerr << "--prepare-once requires --enable-sess (prepared handles live on a session)"
              << std::endl;
    std::exit(1);
  }

  std::cout << "MsgSize(B)  Batch  Iters  AvgBW(GB/s)  AvgLat(us)  TotalDur(us)" << std::endl;

  for (auto& planEntry : plan) {
    const size_t msg = planEntry.first;
    const int curBatch = planEntry.second;
    // Two ways to move curBatch transfers per iteration, mirroring the Python bench:
    //   --enable-batch-transfer (default, batched): ONE N-descriptor batch request
    //     (BatchWrite/BatchRead) -- the nixl-equivalent (nixl always batches).
    //   --disable-batch-transfer (singles): curBatch INDIVIDUAL single-transfer
    //     submissions per iteration (Python run_single_once), each its own status.
    // batch==1 is a single transfer either way.
    const bool batched = a.enable_batch_transfer && curBatch > 1;
    const int perSlot = batched ? 1 : curBatch;  // statuses per request
    // Strict stop-and-wait: one request outstanding at a time (nixl
    // --pipeline_depth 1). [perSlot] status array (TransferStatus is
    // non-copyable, so a flat vector rather than nested).
    std::vector<TransferStatus> st(static_cast<size_t>(perSlot));

    // Batch layout for the batched path. Strided (default): transfer i at
    // (msg+1)*i so the N transfers stay SEPARATE WRs (stresses SQ, real batching)
    // -- matches Python default. Contiguous (--batch-contiguous): transfer i at
    // msg*i, adjacent, so MORI may merge them into one big WR (fast but not really
    // batching, and hits the 1 GiB max_msg_sz without chunking).
    const size_t stride = a.batch_contiguous ? msg : (msg + 1);
    SizeVec offsets(curBatch), sizes(curBatch);
    for (int i = 0; i < curBatch; ++i) {
      offsets[i] = static_cast<size_t>(i) * stride;
      sizes[i] = msg;
    }
    // Engine (non-session) batch path needs vec-of-vec + desc vectors. These and
    // the status/id vectors below are built once per sweep point and reused every
    // iteration: allocating them inside post() would charge two heap
    // allocation/free pairs per iteration to the reported latency.
    MemDescVec locVec{localMem}, remVec{peerMem};
    BatchSizeVec offVec{offsets}, sizeVec{sizes};
    TransferStatusPtrVec statusPtrs{&st[0]};
    TransferUniqueIdVec batchIds(1);

    // --prepare-once: build the merged/chunked WR list ONCE per (msg,batch) so
    // the timed loop only re-posts it (nixl createXferReq / postXferReq split).
    // Batched => a single N-descriptor handle; singles => one handle per
    // transfer. Session-only (guarded above).
    std::vector<std::shared_ptr<PreparedTransfer>> prepared;
    if (a.prepare_once) {
      auto build = [&](const SizeVec& lo, const SizeVec& ro, const SizeVec& sz) {
        auto h = sessPtr->PrepareBatch(lo, ro, sz, isRead);
        if (!h) {
          std::cerr << "PrepareBatch failed (msg=" << msg << ", batch=" << curBatch << ")"
                    << std::endl;
          std::exit(1);
        }
        prepared.push_back(std::move(h));
      };
      if (batched) {
        build(offsets, offsets, sizes);
      } else {
        for (int i = 0; i < curBatch; ++i) {
          const size_t off = static_cast<size_t>(i) * msg;
          build(SizeVec{off}, SizeVec{off}, SizeVec{msg});
        }
      }
    }

    auto post = [&]() {
      TransferStatus* base = &st[0];
      for (int i = 0; i < perSlot; ++i) base[i].SetCode(StatusCode::INIT);
      if (a.prepare_once) {
        // Re-post the pre-built handle(s); no sort/merge/chunk on the hot path.
        if (batched) {
          TransferUniqueId id = sessPtr->AllocateTransferUniqueId();
          sessPtr->PostPrepared(prepared[0], &base[0], id);
        } else {
          for (int i = 0; i < curBatch; ++i) {
            TransferUniqueId id = sessPtr->AllocateTransferUniqueId();
            sessPtr->PostPrepared(prepared[static_cast<size_t>(i)], &base[i], id);
          }
        }
        return;
      }
      if (batched) {
        // ONE N-descriptor batch request (nixl / Python --enable-batch-transfer).
        TransferUniqueId id =
            useSess ? sessPtr->AllocateTransferUniqueId() : engine.AllocateTransferUniqueId();
        if (useSess) {
          if (isRead)
            sessPtr->BatchRead(offsets, offsets, sizes, &base[0], id);
          else
            sessPtr->BatchWrite(offsets, offsets, sizes, &base[0], id);
        } else {
          statusPtrs[0] = &base[0];
          batchIds[0] = id;
          if (isRead)
            engine.BatchRead(locVec, offVec, remVec, offVec, sizeVec, statusPtrs, batchIds);
          else
            engine.BatchWrite(locVec, offVec, remVec, offVec, sizeVec, statusPtrs, batchIds);
        }
      } else {
        // curBatch individual single-transfer submissions (Python run_single_once);
        // contiguous offsets i*msg, matching Python's single path. curBatch==1 is a
        // single transfer at offset 0.
        for (int i = 0; i < curBatch; ++i) {
          const size_t off = static_cast<size_t>(i) * msg;
          TransferUniqueId id =
              useSess ? sessPtr->AllocateTransferUniqueId() : engine.AllocateTransferUniqueId();
          if (useSess) {
            if (isRead)
              sessPtr->Read(off, off, msg, &base[i], id);
            else
              sessPtr->Write(off, off, msg, &base[i], id);
          } else {
            if (isRead)
              engine.Read(localMem, off, peerMem, off, msg, &base[i], id);
            else
              engine.Write(localMem, off, peerMem, off, msg, &base[i], id);
          }
        }
      }
    };

    // The request is complete only when ALL perSlot sub-transfers have left
    // INIT/IN_PROGRESS (mirrors Python waiting on the whole status_list). Spin,
    // like the timed loop. reqFailed returns the first failed status, or nullptr.
    auto reqDone = [&]() {
      TransferStatus* base = &st[0];
      for (int i = 0; i < perSlot; ++i)
        if (base[i].InProgress() || base[i].Init()) return false;
      return true;
    };
    auto reqFailed = [&]() -> TransferStatus* {
      TransferStatus* base = &st[0];
      for (int i = 0; i < perSlot; ++i)
        if (base[i].Failed()) return &base[i];
      return nullptr;
    };

    // ---- warmup (excluded from timing) ----
    for (int w = 0; w < a.warmup; ++w) {
      post();
      while (!reqDone()) { /* spin: CQ worker thread stores status */
      }
      if (TransferStatus* f = reqFailed()) {
        std::cerr << "warmup transfer failed: " << f->Message() << std::endl;
        std::exit(1);
      }
    }

    // ---- timed region: ONE whole-loop timer, nixl-style ----
    // Strict stop-and-wait (nixl --pipeline_depth 1): post one request, spin
    // until it completes, then post the next.
    auto t0 = Clock::now();

    for (int completed = 0; completed < a.iters; ++completed) {
      post();
      // spin-poll, mirroring nixl's "check status, if IN_PROG continue" scan
      // (status flags stored by MORI's CQ worker thread)
      while (!reqDone()) { /* spin */
      }
      if (TransferStatus* f = reqFailed()) {
        std::cerr << "transfer failed: " << f->Message() << std::endl;
        std::exit(1);
      }
    }
    auto t1 = Clock::now();

    double total_us =
        std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(t1 - t0).count();
    // nixl parity (nixl_worker/utils.cpp): count block_size*batch_size*num_iter
    // bytes over ONE whole-loop timer, and report latency PER SINGLE TRANSFER.
    // Both batched and singles modes move curBatch transfers of `msg` per iter:
    //   total_bytes  = msg * curBatch * iters
    //   avg_bw       = total_bytes/1e9 / (total_us/1e6)                 [GB/s, GB=10^9]
    //   avg_latency  = total_us / (iters * curBatch)                    [us per transfer]
    // matching nixl's avg_latency = total_duration/(per_thread_iter*batch_size).
    const int effBatch = curBatch;
    const double numXfers = static_cast<double>(a.iters) * effBatch;
    double total_bytes = static_cast<double>(msg) * numXfers;
    double avg_bw = (total_bytes / 1e9) / (total_us / 1e6);  // GB/s, GB=10^9
    double avg_lat = total_us / numXfers;                    // us per single transfer

    std::printf("%-11zu %-6d %-6d %-12.2f %-11.2f %-.1f\n", msg, effBatch, a.iters, avg_bw, avg_lat,
                total_us);
    std::fflush(stdout);
  }

  rdv.Barrier();  // tell target we're done

  // Correctness check on the last sweep point, matching the Python benchmark's
  // always-on validation. Runs after the timed region so it cannot perturb the
  // reported numbers.
  const auto& last = plan.back();
  const bool valid = ValidateTransfer(rdv, buf, last.first, last.second,
                                      a.enable_batch_transfer && last.second > 1,
                                      a.batch_contiguous, a.rank, !a.skip_validate);

  // Tear down in reverse order of construction: the session references the
  // registered region, and the registration references the HIP allocation, so
  // both must go before the pages are released.
  sessOpt.reset();
  engine.DeregisterMemory(localMem);
  if (cpuMem)
    HIP_CHECK(hipHostFree(buf));
  else
    HIP_CHECK(hipFree(buf));
  return valid ? 0 : 1;
}

int main(int argc, char** argv) {
  // Both the bootstrap layer and the IO engine report failures by throwing, so
  // unwinding here runs the destructors instead of leaving teardown to an exit
  // path.
  try {
    return RunBenchmark(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "bench_engine: " << e.what() << std::endl;
    return 1;
  }
}
