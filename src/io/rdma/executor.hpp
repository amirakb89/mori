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

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#include "mori/application/transport/rdma/rdma.hpp"
#include "mori/io/common.hpp"
#include "src/io/rdma/common.hpp"

namespace mori {
namespace io {

/* ---------------------------------------------------------------------------------------------- */
/*                                         Data Structures                                        */
/* ---------------------------------------------------------------------------------------------- */
struct ExecutorReq {
  const EpPairVec& eps;
  const application::RdmaMemoryRegion& local;
  const SizeVec& localOffsets;
  const application::RdmaMemoryRegion& remote;
  const SizeVec& remoteOffsets;
  const SizeVec& sizes;
  std::shared_ptr<CqCallbackMeta> callbackMeta;
  TransferUniqueId id;
  int postBatchSize;
  bool isRead;
  size_t chunkBytes{0};
  int maxChunks{1};
};

// Prepared (build-once, post-many) counterpart of ExecutorReq. PrepareBatch has
// already split the batch into one self-contained single-endpoint WR list per
// slice, so a worker only re-arms and posts its own slice: slices share no mutable
// state, and the endpoint a slice lands on is chosen per post by the caller.
struct ExecutorPreparedReq {
  const EpPairVec& eps;
  const application::RdmaMemoryRegion& local;
  const application::RdmaMemoryRegion& remote;
  std::vector<PreparedRdmaBatch>& slices;
  std::shared_ptr<CqCallbackMeta> callbackMeta;
  TransferUniqueId id;
};

// Split `totalBatchSize` descriptors into at most min(numEps, numWorker) contiguous
// [begin, end) slices. Shared by the build-and-post path and the prepared path so
// that both decompose a batch identically at a given worker count.
std::vector<std::pair<int, int>> SplitBatchWork(int numEps, int numWorker, int totalBatchSize);

/* ---------------------------------------------------------------------------------------------- */
/*                                            Executor                                            */
/* ---------------------------------------------------------------------------------------------- */
class Executor {
 public:
  Executor() = default;
  virtual ~Executor() = default;

  virtual void Start() = 0;
  virtual void Shutdown() = 0;
  virtual RdmaOpRet RdmaBatchReadWrite(const ExecutorReq& req) = 0;
  virtual RdmaOpRet PostPrepared(const ExecutorPreparedReq& req) = 0;
  // Number of worker threads actually available, which PrepareBatch needs to
  // decide how many slices to build.
  virtual int NumWorkers() const = 0;
};

/* ---------------------------------------------------------------------------------------------- */
/*                                       MultithreadExecutor                                      */
/* ---------------------------------------------------------------------------------------------- */
class MultithreadExecutor : public Executor {
 public:
  MultithreadExecutor(int numWorker);
  ~MultithreadExecutor();

  RdmaOpRet RdmaBatchReadWrite(const ExecutorReq& req);
  RdmaOpRet PostPrepared(const ExecutorPreparedReq& req);
  int NumWorkers() const { return numWorker; }
  void Start();
  void Shutdown();

 private:
  struct Task {
    // Exactly one of `req` (build a descriptor range, then post it) or `preq`
    // (post an already-built slice) is set.
    const ExecutorReq* req{nullptr};
    const ExecutorPreparedReq* preq{nullptr};
    int epId{-1};
    int begin{-1};
    int end{-1};
    int sliceId{-1};
    std::promise<RdmaOpRet> ret;

    Task(const ExecutorReq* req_, int epId_, int begin_, int end_)
        : req(req_), epId(epId_), begin(begin_), end(end_) {}
    Task(const ExecutorPreparedReq* preq_, int epId_, int sliceId_)
        : preq(preq_), epId(epId_), sliceId(sliceId_) {}
  };

  std::vector<std::pair<int, int>> SplitWork(const ExecutorReq& req);
  static RdmaOpRet AggregateResults(std::vector<std::future<RdmaOpRet>>& futs);

  class Worker {
   public:
    Worker(int wid);
    ~Worker();
    void MainLoop();
    void Start();
    void Shutdown();

    void Submit(Task&&);

   private:
    int workerId{-1};
    std::atomic<bool> running{false};
    mutable std::mutex mu;
    std::condition_variable cond;
    std::queue<Task> q;
    std::thread thd;
  };

 public:
  int numWorker{1};

 private:
  std::atomic<bool> running{false};
  std::vector<std::unique_ptr<Worker>> pool;
};

}  // namespace io
}  // namespace mori
