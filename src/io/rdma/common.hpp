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
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mori/io/common.hpp"
#include "mori/io/enum.hpp"
#include "mori/io/msgpack_adaptor.hpp"
#include "src/io/call_diagnostics_internal.hpp"

namespace mori {
namespace io {

/* ---------------------------------------------------------------------------------------------- */
/*                                     Common Data Structures                                     */
/* ---------------------------------------------------------------------------------------------- */
struct TopoKey {
  int deviceId;
  MemoryLocationType loc;
  int numaNode{-1};

  bool operator==(const TopoKey& rhs) const noexcept {
    return (deviceId == rhs.deviceId) && (loc == rhs.loc) && (numaNode == rhs.numaNode);
  }

  MSGPACK_DEFINE(deviceId, loc, numaNode);
};

struct TopoKeyPair {
  TopoKey local;
  TopoKey remote;

  bool operator==(const TopoKeyPair& rhs) const noexcept {
    return (local == rhs.local) && (remote == rhs.remote);
  }

  MSGPACK_DEFINE(local, remote);
};

struct MemoryKey {
  int devId;
  MemoryUniqueId id;

  bool operator==(const MemoryKey& rhs) const noexcept {
    return (id == rhs.id) && (devId == rhs.devId);
  }
};

}  // namespace io
}  // namespace mori

namespace std {
template <>
struct hash<mori::io::TopoKey> {
  std::size_t operator()(const mori::io::TopoKey& k) const noexcept {
    std::size_t h1 = std::hash<uint32_t>{}(k.deviceId);
    std::size_t h2 = std::hash<uint32_t>{}(static_cast<uint32_t>(k.loc));
    std::size_t h3 = std::hash<int>{}(k.numaNode);
    std::size_t seed = h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    return seed ^ (h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2));
  }
};

template <>
struct hash<mori::io::TopoKeyPair> {
  std::size_t operator()(const mori::io::TopoKeyPair& kp) const noexcept {
    std::size_t h1 = std::hash<mori::io::TopoKey>{}(kp.local);
    std::size_t h2 = std::hash<mori::io::TopoKey>{}(kp.remote);
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
  }
};

template <>
struct hash<mori::io::MemoryKey> {
  std::size_t operator()(const mori::io::MemoryKey& k) const noexcept {
    std::size_t h1 = std::hash<mori::io::MemoryUniqueId>{}(k.id);
    std::size_t h2 = std::hash<int>{}(k.devId);
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
  }
};

}  // namespace std

namespace mori {
namespace io {

struct NotifMessage {
  TransferUniqueId id{0};
  int qpIndex{-1};
  int totalNum{-1};
};

// wr_id namespace:
//   Zone A: notification RECV indices [0, notifPerQp)
//   Zone B: ledger record IDs [notifPerQp, 2^63)
//   Zone C: notification SEND IDs tagged with bit 63
static constexpr uint64_t kNotifSendWrIdTag = uint64_t{1} << 63;

inline bool IsNotifSendWrId(uint64_t wr_id) { return (wr_id & kNotifSendWrIdTag) != 0; }

inline TransferUniqueId ExtractTransferIdFromWrId(uint64_t wr_id) {
  return static_cast<TransferUniqueId>(wr_id & ~kNotifSendWrIdTag);
}

uint64_t MakeNotifSendWrId(TransferUniqueId id);

std::vector<std::pair<uint64_t, uint32_t>> PlanChunks(uint32_t total, size_t chunkBytes,
                                                      int maxChunks);

struct ChunkedSgeSegment {
  uint64_t remoteOffset{0};
  uint64_t localAddr{0};
  uint32_t length{0};
};

struct ChunkGeometry {
  uint64_t finalCount{0};
  uint64_t targetChunkBytes{0};
};

ChunkGeometry PlanChunkGeometry(uint64_t totalLength, size_t chunkBytes, int maxChunks,
                                uint64_t maxMessageSize);

uint64_t CountChunksForSize(uint64_t totalLength, size_t chunkBytes, int maxChunks,
                            uint64_t maxMessageSize);

void PlanSgeStreamChunks(std::vector<ChunkedSgeSegment>& plan, const std::vector<ibv_sge>& sges,
                         uint64_t totalLength, size_t chunkBytes, int maxChunks,
                         uint64_t maxMessageSize);

struct CqCallbackMeta {
  CqCallbackMeta(TransferStatus* s, TransferUniqueId id_, int n)
      : status(s), id(id_), totalBatchSize(n) {}

  TransferStatus* status{nullptr};
  TransferUniqueId id{0};
  int totalBatchSize{0};
  std::atomic<uint32_t> finishedBatchSize{0};
  internal::IoCallDiagnostics diagnostics{};
};

// SubmissionLedger: tracks per-EP WR submissions and enables precise sqDepth release.
enum class SubmissionState : uint8_t {
  Posted,    // submitted, awaiting CQE
  Orphaned,  // partial post without signaled tail; awaits recovery
};

struct SubmissionRecord {
  uint64_t recordId{0};
  int postedWr{0};
  bool hasSignaledTail{false};
  SubmissionState state{SubmissionState::Posted};
  std::shared_ptr<CqCallbackMeta> meta;
  int batchSize{0};
};

class SubmissionLedger {
 public:
  explicit SubmissionLedger(uint32_t notifPerQp) : nextId_{notifPerQp} {}

  // Allocate recordId, insert Posted record, return recordId.
  uint64_t Insert(int postedWr, bool hasSignaledTail, std::shared_ptr<CqCallbackMeta> meta,
                  int batchSize);

  // Insert an Orphaned record (partial post, no signaled tail).
  void InsertOrphaned(int postedWr, std::shared_ptr<CqCallbackMeta> meta, int batchSize);

  // CQE path: find record by recordId, release sqDepth, return CqCallbackMeta.
  // Returns nullptr if record not found.
  std::shared_ptr<CqCallbackMeta> ReleaseByCqe(uint64_t recordId, std::atomic<int>* sqDepth,
                                               int* outBatchSize);

  // Recovery path: release only Orphaned records and keep Posted records.
  int ReleaseOrphanedByRecovery(std::atomic<int>* sqDepth);

  bool HasOrphaned() const;

 private:
  mutable std::mutex mu_;
  uint64_t nextId_;
  std::unordered_map<uint64_t, SubmissionRecord> records_;
};

inline constexpr std::memory_order kSqAdmissionOrder = std::memory_order_seq_cst;

struct SqAdmissionEvent {
  alignas(4) std::atomic<uint32_t> epoch{0};
  std::atomic<uint32_t> waiters{0};
};

struct EpPair {
  int weight;
  int ldevId;
  int rdevId;
  EngineKey remoteEngineKey;
  application::RdmaEndpoint local;
  application::RdmaEndpointHandle remote;
  // Shared across EpPair copies that refer to the same QP.
  std::shared_ptr<std::atomic<int>> sqDepth;
  int maxSqDepth{0};
  // Degraded flag — set on partial post without signaled tail.
  std::shared_ptr<std::atomic<bool>> degraded;
  std::shared_ptr<SubmissionLedger> ledger;
  // Shared across EpPair copies that refer to the same QP.
  std::shared_ptr<SqAdmissionEvent> sqAdmission;
};

using EndpointId = uint64_t;

struct EndpointRuntime {
  EndpointRuntime() = default;
  EndpointRuntime(EndpointId id_, const EpPair& ep_) : id(id_), ep(ep_) {}

  EndpointId id{0};
  EpPair ep;
};

using EpPairVec = std::vector<EpPair>;
using RouteTable = std::unordered_map<TopoKeyPair, EpPairVec>;
using MemoryTable = std::unordered_map<MemoryKey, application::RdmaMemoryRegion>;

struct RemoteEngineMeta {
  EngineKey key;
  RouteTable rTable;
  MemoryTable mTable;
};

struct RdmaOpRet {
  StatusCode code{StatusCode::INIT};
  std::string message;

  bool Init() { return code == StatusCode::INIT; }
  bool InProgress() { return code == StatusCode::IN_PROGRESS; }
  bool Succeeded() { return code == StatusCode::SUCCESS; }
  bool Failed() { return code > StatusCode::ERR_BEGIN; }
};

void NotifySqStateChanged(const EpPair& ep);

RdmaOpRet RdmaNotifyTransfer(const EpPairVec& eps, TransferStatus* status, TransferUniqueId id);

namespace detail {

// Test hook for SQ admission without proceeding to ibv_post_send on a fake QP.
bool TryReserveSqDepthForTesting(const EpPair& ep, int wrCount, std::string* errMsg = nullptr);

}  // namespace detail

struct RdmaTransferControl {
  size_t chunkBytes{0};
  int maxChunks{1};
  bool creditByWrCount{false};
  bool ownsTotalBatchSize{true};
  bool disableMerge{false};
};

// A single (possibly merged / chunked) RDMA work request plus its scatter-gather
// list. Built once from a batch description and reused for posting. Defined here
// (rather than privately in common.cpp) so it can be cached in a PreparedRdmaBatch.
struct MergedWorkRequest {
  ibv_send_wr wr{};
  std::vector<ibv_sge> sges;
  size_t totalRemoteLength = 0;
  size_t mergedRequests = 1;
};

// Result of the "build" phase of a batch transfer: the fully merged/chunked WR
// list with per-element addresses/lengths resolved, but WITHOUT the per-post
// state (lkey/rkey, wr_id, signaling, next pointers). Reused across posts by
// PostMergedWorkRequests, which re-arms that per-post state each time. This lets
// callers hoist the sort/merge/chunk cost out of a hot transfer loop (the NIXL
// createXferReq / postXferReq split), gated by the caller.
struct PreparedRdmaBatch {
  std::vector<MergedWorkRequest> mergedWrs;
  size_t mergedWrCount{0};
  // Original request count; becomes callbackMeta totalBatchSize when
  // !creditByWrCount (the chunked path overwrites it with the final WR count).
  size_t batchSize{0};
  RdmaTransferControl control{};
  int postBatchSize{-1};
  bool isRead{false};
  bool valid{false};
};

// Build phase: produce the merged/chunked WR list for the given batch into the
// caller-provided scratch pools. On success, `outWrs` points at whichever pool
// holds the final list and `outCount` is its length. Used by both the original
// RdmaBatchReadWrite (thread-local pools) and the prepared path (owned vectors).
RdmaOpRet BuildMergedWorkRequests(const EpPairVec& eps,
                                  const application::RdmaMemoryRegion& baseLocalMr,
                                  const application::RdmaMemoryRegion& baseRemoteMr,
                                  const SizeVec& localOffsets, const SizeVec& remoteOffsets,
                                  const SizeVec& sizes, bool isRead,
                                  const RdmaTransferControl& control, std::vector<size_t>& indices,
                                  std::vector<MergedWorkRequest>& mergedPool,
                                  std::vector<MergedWorkRequest>& chunkedPool,
                                  std::vector<ChunkedSgeSegment>& chunkPlan,
                                  std::vector<MergedWorkRequest>*& outWrs, size_t& outCount);

// Post phase: re-arm per-post state (lkey/rkey, wr_id, signaling, next) on the
// merged WR list and submit via ibv_post_send. Safe to call repeatedly on the
// same `mergedWrs` (reset-on-post semantics), which is what the prepared path does.
RdmaOpRet PostMergedWorkRequests(const EpPairVec& eps,
                                 const std::vector<application::RdmaMemoryRegion>& localMrPerEp,
                                 const std::vector<application::RdmaMemoryRegion>& remoteMrPerEp,
                                 std::vector<MergedWorkRequest>& mergedWrs, size_t mergedWrCount,
                                 std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id,
                                 int postBatchSize, const RdmaTransferControl& control);

// Prepared-transfer helpers: build once, post many. BuildPreparedRdmaBatch runs
// the build phase and stores the owned WR list in `out`. PostPreparedRdmaBatch
// re-posts it. Used both for inline posting (one list spanning every endpoint) and
// for worker posting, where each worker owns a single-endpoint list.
RdmaOpRet BuildPreparedRdmaBatch(const EpPairVec& eps,
                                 const application::RdmaMemoryRegion& baseLocalMr,
                                 const application::RdmaMemoryRegion& baseRemoteMr,
                                 const SizeVec& localOffsets, const SizeVec& remoteOffsets,
                                 const SizeVec& sizes, bool isRead,
                                 const RdmaTransferControl& control, int postBatchSize,
                                 PreparedRdmaBatch& out);

RdmaOpRet PostPreparedRdmaBatch(const EpPairVec& eps,
                                const std::vector<application::RdmaMemoryRegion>& localMrPerEp,
                                const std::vector<application::RdmaMemoryRegion>& remoteMrPerEp,
                                PreparedRdmaBatch& prepared,
                                std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id);

RdmaOpRet RdmaBatchReadWrite(const EpPairVec& eps,
                             const std::vector<application::RdmaMemoryRegion>& localMrPerEp,
                             const std::vector<application::RdmaMemoryRegion>& remoteMrPerEp,
                             const SizeVec& localOffsets, const SizeVec& remoteOffsets,
                             const SizeVec& sizes, std::shared_ptr<CqCallbackMeta> callbackMeta,
                             TransferUniqueId id, bool isRead, int postBatchSize,
                             const RdmaTransferControl& control);

RdmaOpRet RdmaBatchReadWrite(const EpPairVec& eps,
                             const std::vector<application::RdmaMemoryRegion>& localMrPerEp,
                             const std::vector<application::RdmaMemoryRegion>& remoteMrPerEp,
                             const SizeVec& localOffsets, const SizeVec& remoteOffsets,
                             const SizeVec& sizes, std::shared_ptr<CqCallbackMeta> callbackMeta,
                             TransferUniqueId id, bool isRead, int postBatchSize = -1,
                             size_t chunkBytes = 0, int maxChunks = 1,
                             bool creditByWrCount = false);

RdmaOpRet RdmaBatchReadWrite(const EpPairVec& eps, const application::RdmaMemoryRegion& local,
                             const SizeVec& localOffsets,
                             const application::RdmaMemoryRegion& remote,
                             const SizeVec& remoteOffsets, const SizeVec& sizes,
                             std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id,
                             bool isRead, int postBatchSize = -1);

inline RdmaOpRet RdmaBatchRead(const EpPairVec& eps, const application::RdmaMemoryRegion& local,
                               const SizeVec& localOffsets,
                               const application::RdmaMemoryRegion& remote,
                               const SizeVec& remoteOffsets, const SizeVec& sizes,
                               std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id,
                               int postBatchSize = -1) {
  return RdmaBatchReadWrite(eps, local, localOffsets, remote, remoteOffsets, sizes, callbackMeta,
                            id, true /*isRead */, postBatchSize);
}

inline RdmaOpRet RdmaBatchWrite(const EpPairVec& eps, const application::RdmaMemoryRegion& local,
                                const SizeVec& localOffsets,
                                const application::RdmaMemoryRegion& remote,
                                const SizeVec& remoteOffsets, const SizeVec& sizes,
                                std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id,
                                int postBatchSize = -1) {
  return RdmaBatchReadWrite(eps, local, localOffsets, remote, remoteOffsets, sizes, callbackMeta,
                            id, false /*isRead */, postBatchSize);
}

inline RdmaOpRet RdmaReadWrite(const EpPairVec& eps, const application::RdmaMemoryRegion& local,
                               size_t localOffset, const application::RdmaMemoryRegion& remote,
                               size_t remoteOffset, size_t size,
                               std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id,
                               bool isRead) {
  return RdmaBatchReadWrite(eps, local, {localOffset}, remote, {remoteOffset}, {size}, callbackMeta,
                            id, isRead, 1);
}

inline RdmaOpRet RdmaRead(const EpPairVec& eps, const application::RdmaMemoryRegion& local,
                          size_t localOffset, const application::RdmaMemoryRegion& remote,
                          size_t remoteOffset, size_t size,
                          std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id) {
  return RdmaReadWrite(eps, local, localOffset, remote, remoteOffset, size, callbackMeta, id, true);
}

inline RdmaOpRet RdmaWrite(const EpPairVec& eps, const application::RdmaMemoryRegion& local,
                           size_t localOffset, const application::RdmaMemoryRegion& remote,
                           size_t remoteOffset, size_t size,
                           std::shared_ptr<CqCallbackMeta> callbackMeta, TransferUniqueId id) {
  return RdmaReadWrite(eps, local, localOffset, remote, remoteOffset, size, callbackMeta, id,
                       false);
}
}  // namespace io
}  // namespace mori
