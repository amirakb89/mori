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
#include "src/io/rdma/executor.hpp"

#include <pthread.h>
#include <sched.h>

#include <cstring>
#include <future>
#include <vector>

#include "mori/io/logging.hpp"
#include "mori/utils/env_utils.hpp"

namespace mori {
namespace io {

namespace {

// Allowed CPUs of the current process, sorted ascending. Reflects cgroup/cpuset
// limits. Empty means affinity could not be read.
std::vector<int> GetAllowedCpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  std::vector<int> cpus;
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    return cpus;
  }
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
  }
  return cpus;
}

}  // namespace

/* ---------------------------------------------------------------------------------------------- */
/*                                   MultithreadExecutor::Worker                                  */
/* ---------------------------------------------------------------------------------------------- */
MultithreadExecutor::Worker::Worker(int wid) : workerId(wid) {}

MultithreadExecutor::Worker::~Worker() { Shutdown(); }

void MultithreadExecutor::Worker::Start() {
  if (running.load()) return;
  running.store(true);
  thd = std::thread([this] { MainLoop(); });
}

void MultithreadExecutor::Worker::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!running.load()) return;
    running.store(false);
    cond.notify_all();
  }
  if (thd.joinable()) thd.join();
}

void MultithreadExecutor::Worker::MainLoop() {
  // MORI_CORE_OFFSET is relative to the allowed CPU list, so binding stays within the cpuset.
  if (auto coreOffset = mori::env::GetInt("MORI_CORE_OFFSET")) {
    std::vector<int> allowed = GetAllowedCpus();
    if (allowed.empty()) {
      MORI_IO_WARN(
          "worker {} could not read allowed CPU set (sched_getaffinity failed); "
          "worker will run on any available core.",
          workerId);
    } else {
      int n = static_cast<int>(allowed.size());
      int idx = ((workerId + *coreOffset) % n + n) % n;
      int targetCore = allowed[idx];

      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(targetCore, &cpuset);

      int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
      if (rc != 0) {
        MORI_IO_WARN(
            "worker {} failed to set affinity to core {} (allowed[{}], allowed size {}): "
            "errno={} ({}). Worker will run on any available core. "
            "This is usually caused by NUMA configuration or container CPU limits.",
            workerId, targetCore, idx, n, rc, strerror(rc));
      } else {
        MORI_IO_INFO("worker {} bound to core {} (allowed[{}] of {} allowed CPUs, offset {})",
                     workerId, targetCore, idx, n, *coreOffset);
      }
    }
  }

  MORI_IO_INFO("worker {} enter main loop, running on core {}", workerId, sched_getcpu());

  Task task{nullptr, 0, 0, 0};
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mu);
      cond.wait(lock, [this]() { return !q.empty() || !running.load(); });

      if (!running.load()) {
        MORI_IO_INFO("worker {} shutdown", workerId);
        break;
      }
      task = std::move(q.front());
      q.pop();
    }

    thread_local std::vector<application::RdmaMemoryRegion> localMrPerEp(1);
    thread_local std::vector<application::RdmaMemoryRegion> remoteMrPerEp(1);

    if (task.preq != nullptr) {
      // Prepared slice: the WR list was built by PrepareBatch, so only the
      // per-post state (lkey/rkey, wr_id, signaling, next chain) is re-armed here.
      localMrPerEp[0] = task.preq->local;
      remoteMrPerEp[0] = task.preq->remote;

      RdmaOpRet ret = mori::io::PostPreparedRdmaBatch(
          {task.preq->eps[task.epId]}, localMrPerEp, remoteMrPerEp, task.preq->slices[task.sliceId],
          task.preq->callbackMeta, task.preq->id);
      task.ret.set_value(ret);
      MORI_IO_TRACE("Worker {} post prepared task {} slice {} ep {} ret code {}", workerId,
                    task.preq->id, task.sliceId, task.epId, static_cast<uint32_t>(ret.code));
      continue;
    }

    SizeVec tLoclOffsets(task.req->localOffsets.begin() + task.begin,
                         task.req->localOffsets.begin() + task.end);
    SizeVec tRemoteOffsets(task.req->remoteOffsets.begin() + task.begin,
                           task.req->remoteOffsets.begin() + task.end);
    SizeVec tSizes(task.req->sizes.begin() + task.begin, task.req->sizes.begin() + task.end);

    const bool chunk = task.req->chunkBytes > 0;
    RdmaTransferControl control{};
    control.chunkBytes = task.req->chunkBytes;
    control.maxChunks = task.req->maxChunks;
    control.creditByWrCount = chunk;
    control.ownsTotalBatchSize = false;
    control.disableMerge = chunk;

    localMrPerEp[0] = task.req->local;
    remoteMrPerEp[0] = task.req->remote;

    RdmaOpRet ret = mori::io::RdmaBatchReadWrite(
        {task.req->eps[task.epId]}, localMrPerEp, remoteMrPerEp, tLoclOffsets, tRemoteOffsets,
        tSizes, task.req->callbackMeta, task.req->id, task.req->isRead, task.req->postBatchSize,
        control);
    task.ret.set_value(ret);
    MORI_IO_TRACE("Worker {} execute task {} begin {} end {} ret code {}", workerId, task.req->id,
                  task.begin, task.end, static_cast<uint32_t>(ret.code));
  }
}

void MultithreadExecutor::Worker::Submit(Task&& task) {
  MORI_IO_FUNCTION_TIMER;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!running.load()) {
      task.ret.set_value({StatusCode::ERR_BAD_STATE, "worker not started yet"});
      return;
    }
    q.push(std::move(task));
    cond.notify_all();
  }
  MORI_IO_TRACE("Submit to worker {} task {} begin {} end {}", workerId, task.req->id, task.begin,
                task.end);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                       MultithreadExecutor                                      */
/* ---------------------------------------------------------------------------------------------- */
MultithreadExecutor::MultithreadExecutor(int n) : numWorker(n) {
  assert(n > 0);
  for (int i = 0; i < numWorker; i++) {
    pool.emplace_back(new Worker(i));
  }
}

MultithreadExecutor::~MultithreadExecutor() { Shutdown(); }

std::vector<std::pair<int, int>> SplitBatchWork(int numEps, int numWorker, int totalBatchSize) {
  assert(numEps > 0);

  int numActiveWorkers = std::min(numEps, numWorker);
  int perWorkerBatchSize = (totalBatchSize + numActiveWorkers - 1) / numActiveWorkers;

  std::vector<std::pair<int, int>> splits;
  for (int i = 0; i < numActiveWorkers; i++) {
    int begin = i * perWorkerBatchSize;
    int end = std::min(begin + perWorkerBatchSize, totalBatchSize);
    splits.push_back({begin, end});
    if (end >= totalBatchSize) break;
  }

  return splits;
}

std::vector<std::pair<int, int>> MultithreadExecutor::SplitWork(const ExecutorReq& req) {
  return SplitBatchWork(static_cast<int>(req.eps.size()), numWorker,
                        static_cast<int>(req.sizes.size()));
}

RdmaOpRet MultithreadExecutor::AggregateResults(std::vector<std::future<RdmaOpRet>>& futs) {
  bool hasFail = false;
  int numSucc = 0;
  RdmaOpRet failedRet;
  for (auto& fut : futs) {
    RdmaOpRet ret = fut.get();
    if (ret.Failed()) {
      hasFail = true;
      failedRet = ret;
    } else if (ret.Succeeded()) {
      numSucc++;
    }
  }
  if (hasFail) return failedRet;

  if (numSucc == static_cast<int>(futs.size())) {
    return {StatusCode::SUCCESS, ""};
  }
  return {StatusCode::IN_PROGRESS, ""};
}

RdmaOpRet MultithreadExecutor::RdmaBatchReadWrite(const ExecutorReq& req) {
  MORI_IO_FUNCTION_TIMER;

  auto splits = SplitWork(req);
  int numSplits = splits.size();
  int numEps = static_cast<int>(req.eps.size());
  // Rotate the starting EP by transfer id so single-segment transfers spread
  // evenly across all QPs instead of always landing on eps[0].
  int epOffset = static_cast<int>(req.id % static_cast<uint64_t>(numEps));
  std::vector<std::future<RdmaOpRet>> futs;

  for (int i = 0; i < numSplits; i++) {
    int epId = (i + epOffset) % numEps;
    Task task{&req, epId, splits[i].first, splits[i].second};
    futs.push_back(std::move(task.ret.get_future()));
    // Keep each QP owned by a stable worker to preserve QP affinity.
    pool[epId % numWorker]->Submit(std::move(task));
  }

  RdmaOpRet ret = AggregateResults(futs);
  MORI_IO_TRACE("MultithreadExecutor submit request for RdmaBatchReadWrite done");
  return ret;
}

RdmaOpRet MultithreadExecutor::PostPrepared(const ExecutorPreparedReq& req) {
  MORI_IO_FUNCTION_TIMER;

  int numSlices = static_cast<int>(req.slices.size());
  int numEps = static_cast<int>(req.eps.size());
  if (numSlices == 0) return {StatusCode::SUCCESS, ""};
  if (numEps == 0) return {StatusCode::ERR_INVALID_ARGS, "no endpoints"};

  // Slices are endpoint-agnostic (addresses come from the session's base MRs and
  // lkey/rkey are re-armed per post), so the same id-based rotation the build path
  // uses still spreads small transfers across every QP.
  int epOffset = static_cast<int>(req.id % static_cast<uint64_t>(numEps));
  std::vector<std::future<RdmaOpRet>> futs;
  futs.reserve(numSlices);

  for (int i = 0; i < numSlices; i++) {
    int epId = (i + epOffset) % numEps;
    Task task{&req, epId, i};
    futs.push_back(std::move(task.ret.get_future()));
    // Keep each QP owned by a stable worker to preserve QP affinity.
    pool[epId % numWorker]->Submit(std::move(task));
  }

  RdmaOpRet ret = AggregateResults(futs);
  MORI_IO_TRACE("MultithreadExecutor submit request for PostPrepared done");
  return ret;
}

void MultithreadExecutor::Start() {
  for (auto& worker : pool) {
    worker->Start();
  }
}

void MultithreadExecutor::Shutdown() {
  for (auto& worker : pool) {
    worker->Shutdown();
  }
}

}  // namespace io
}  // namespace mori
