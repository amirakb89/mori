# MORI-IO architecture for reviewers

What a reviewer must hold in mind to judge a MORI-IO change. Symbols are named,
not line-numbered, so search for them. Written against `origin/main` at
`202946511`.

When the base branch and this file disagree, the base branch is right: fix this
file. When only the change under review disagrees with this file, the change is
the suspect: it either breaks something described here or must update this file
in the same PR.

## Contents

- [Layers](#layers)
- [Entry points and what each validates](#entry-points-and-what-each-validates)
- [Life of an RDMA transfer](#life-of-an-rdma-transfer)
- [Threads](#threads)
- [Locks](#locks)
- [Ownership and lifetime](#ownership-and-lifetime)
- [Invariants](#invariants)
- [How a transfer uses several QPs](#how-a-transfer-uses-several-qps)
- [XGMI and FABRIC](#xgmi-and-fabric)
- [How a status is waited on](#how-a-status-is-waited-on)
- [Tests that pin the invariants](#tests-that-pin-the-invariants)

## Layers

| Layer | Files | Role |
|---|---|---|
| Public API | `include/mori/io/{engine,backend,common,enum,peer_failure}.hpp` | `IOEngine`, `IOEngineSession`, `Backend`, `BackendSession`, `TransferStatus`, `MemoryDesc`, `EngineDesc` |
| Engine | `src/io/engine.cpp` | Backend creation, route selection (`SelectBackend`: XGMI, then FABRIC, then RDMA/OFI/TCP), route cache, `WaitAll` |
| RDMA backend | `src/io/rdma/backend_impl.{hpp,cpp}` | `RdmaManager` (devices, MRs, endpoints), `NotifManager` (CQ poller, notifications), `ControlPlaneServer` (TCP), `RdmaBackendSession`, `RdmaBackend` (session cache, QP replacement) |
| RDMA data path | `src/io/rdma/common.{hpp,cpp}`, `ledger.cpp` | `RdmaBatchReadWrite`, send-queue admission, `SubmissionLedger`, `CqCallbackMeta`, `SettlePendingWrs` |
| RDMA workers | `src/io/rdma/executor.{hpp,cpp}` | `MultithreadExecutor`: one worker thread per QP share |
| RDMA events | `src/io/rdma/async_event_monitor.{hpp,cpp}`, `src/io/peer_failure_tracker.cpp` | Verbs async events, QP/port/CQ/device liveness |
| RDMA wire | `src/io/rdma/protocol.{hpp,cpp}` | TCP control messages `RegEndpoint`, `AskMemoryRegion` (msgpack) |
| Test seam | `src/io/rdma/verbs_ops.hpp`, `fault_injector.{hpp,cpp}` | Post, poll, event and QP-modify calls go through `Verbs()`, so a fault-injection build can intercept them. `ibv_req_notify_cq` and `ibv_ack_cq_events` are called directly |
| Layer below | `src/application/transport/rdma/rdma.cpp`, `src/application/transport/rdma/providers/ibverbs/ibverbs.cpp` | Device list, QP and CQ creation, `ConnectEndpoint` (QP attributes at RTS), the `MORI_IO_QP_*` readers. MORI-IO always uses `RdmaBackendType::IBVerbs` |
| XGMI | `src/io/xgmi/` | HIP IPC + `hipMemcpyPeerAsync`, `StreamPool`, `EventPool` |
| FABRIC | `src/io/fabric/` | UALink vPOD import + JIT copy kernels |
| OFI | `src/io/ofi/` | libfabric RMA, one progress thread |
| Sim | `src/io/sim/` | Timed fake backends, real control plane |
| Python | `src/pybind/pybind_io.cpp`, `python/mori/io/engine.py` | Bindings; the GIL is released in transfer and wait calls |

## Entry points and what each validates

A check covers only the entry points that pass through it.

| Entry point | Path | Checks made on the way |
|---|---|---|
| `IOEngine::Read`, `Write` | `SelectBackend` → `Backend::ReadWrite` → `GetOrCreateSessionCached` → session `ReadWrite` | No backend: status `ERR_BAD_STATE` |
| `IOEngine::BatchRead`, `BatchWrite` | For each item: `SelectBackend` → `Backend::BatchReadWrite` → cached session `BatchReadWrite` | Outer vector lengths: `assert` only. `RdmaBackend::BatchReadWrite`: inner lengths by `assert`; an empty batch gets `SetCode(SUCCESS)` there and never reaches the session |
| `IOEngineSession::Read`, `Write`, `BatchRead`, `BatchWrite` | `backendSess` directly | None above the session: this path skips `SelectBackend` and every `Backend::*` function |
| RDMA session, inline posting | `RdmaBatchReadWrite` | At run time: equal lengths, empty batch (`SUCCESS`), endpoints present, one MR per endpoint, `maxChunks`, each size at most `UINT32_MAX`, range against the first MR, `max_msg_sz` |
| RDMA session, worker posting | `MultithreadExecutor::SplitWork` and `Worker::MainLoop`, then `RdmaBatchReadWrite` for each range | The worker slices all three vectors by indices from `sizes.size()` before `RdmaBatchReadWrite` runs, so its length check always sees equal lengths |
| XGMI session | `XgmiBackendSession::ReadWrite`, `BatchReadWrite` | Lengths by `assert`. No range check: the session holds no sizes |
| Python | `python/mori/io/engine.py` → `src/pybind/pybind_io.cpp` | Lists are passed through unchanged |

In this tree a Release build compiles `src/io` without `-DNDEBUG`
(`CMakeLists.txt` sets `CMAKE_CXX_FLAGS_RELEASE` to `-O3`; confirmed in the
compile flags of a Release build). An `assert` therefore aborts the process in
Release too, and disappears only in a build that defines `NDEBUG` itself.

## Life of an RDMA transfer

1. `IOEngine::Read/Write/BatchRead/BatchWrite` calls `SelectBackend`, then the backend.
2. `RdmaBackend::ReadWrite` calls `GetOrCreateSessionCached`. A first use, or a
   session whose QP failed, runs `CreateSession`: under the per-connection build
   mutex it builds missing QPs through `ControlPlaneServer::BuildRdmaConn`, picks
   `qpPerTransfer` live endpoints and resolves one local and one remote MR for
   each endpoint.
3. `RdmaBackendSession::ReadWrite/BatchReadWrite` sets `IN_PROGRESS`, rejects when
   the local port is down, and creates the `CqCallbackMeta` that carries the
   caller's `TransferStatus*` and the credit total.
4. `RdmaBatchReadWrite` validates, merges adjacent requests into WRs, optionally
   splits them into chunks, and posts batches round-robin over the endpoints. (A
   READ first runs `WarnOnceIfAinicReadIssue`: once in the process it reads the
   firmware version from sysfs and may log.) For
   each batch it reserves send-queue depth, and for a signaled batch it admits a
   ledger record under a `PostGuard` before `ibv_post_send`.
5. `FinishSubmission` accounts the credits that were never posted, completes a
   transfer that posted nothing, and posts the notification SEND when
   notifications are on.
6. The `NotifManager` thread polls the CQs. `ProcessOneCqe` looks up the ledger
   record by `wr_id`, releases send-queue depth and calls `CqCallbackMeta::Account`.
   The `Account` call that covers the last credit claims the status pointer and
   turns the status terminal.
7. A fatal async event reaches `RdmaManager::FailInFlightTransfers`, which marks
   the endpoints degraded and calls `SubmissionLedger::FailAll` on each.

## Threads

| Thread | One per | Runs | Writes to |
|---|---|---|---|
| Caller threads | application | Engine API, session creation, `RdmaBatchReadWrite`, XGMI/FABRIC submit, `TransferStatus::Wait*` | Session caches, ledgers (admission), sqDepth (reserve), statuses |
| `NotifManager::MainLoop` | `RdmaBackend` | `ProcessOneCqe` on every endpoint | sqDepth (release), ledgers (release), statuses (terminal), `notifPool`, `PeerFailureTracker` |
| `ControlPlaneServer::MainLoop` | `RdmaBackend` | Accept, `HandleControlPlaneProtocol` | Endpoints and MRs in `RdmaManager`, `NotifManager` registrations |
| `RdmaAsyncEventMonitor::MainLoop` | `RdmaManager` | Async events, port recheck | `degraded`, ledgers (`FailAll`), statuses (terminal), `portDown_`, `PeerFailureTracker` |
| `MultithreadExecutor::Worker::MainLoop` | worker; `min(qpPerTransfer, numWorkerThreads)` of them | One endpoint's share of a batch | Same as a caller thread, for that endpoint |
| `OfiManager::ProgressLoop` | `OfiBackend` | Drains every OFI CQ, `CompleteOp` | Statuses, frees `OfiOpCtx` |
| Sim server and timer | `SimBackend` | Control plane, timed completion | Statuses |

Two different threads can turn the same RDMA transfer terminal: the CQ poller and
the async event monitor. A submitting thread can do it too. XGMI and FABRIC have
no thread of their own: a status completes only inside the caller's poll or wait.

Sessions are shared between threads. `GetOrCreateSessionCached` hands the same
session object to every caller thread that uses that memory pair, and one
`IOEngineSession` can be used from several threads. A session's members are
written when it is built and only read afterwards; a member that a transfer
writes needs its own synchronization.

Workers exist only when `numWorkerThreads > 1` and `UsesInlineOnly(config)` is
false. Only `RdmaBackendSession::BatchReadWrite` uses them; a single
`ReadWrite` posts inline. Chunked batches use workers only when the local
memory is on a GPU.

## Locks

| Lock | Guards | Notes |
|---|---|---|
| `IOEngine::routeCacheMu` | `routeCache` | `backends`, `memPool` and `desc` have **no** lock |
| `RdmaManager::mu` (shared) | `mTable`, `remotes`, `endpointsById_`, device-context creation | `availDevices` is fixed after construction |
| `PeerFailureTracker::mu_` | All liveness sets and the event queue | Leaf lock |
| `NotifManager::mu` | `registeredRuntimes_`, `notifCtxById_`, `notifPool` | Taken by the CQ poller for each RECV |
| `ControlPlaneServer::mu` | `engines`, `mems` | Held across a TCP connect, and across a read plus MR registration in the `AskMemoryRegion` handler. `eps` is touched only by the server thread |
| `RdmaBackend::sessionCacheMu` | `sessionCache`, `retiredSessions`, `replaceRetryAt` | Not held while a session is created |
| `RdmaBackend::connBuildMapMu_` | `connBuildMu_`, `qpReplacementPacing_` | |
| Per-connection build mutex (`GetConnBuildLock`) | One `CreateSession` at a time for a (remote engine, topology pair) | Held across control-plane round trips |
| `SubmissionLedger::mu_` | `records_`, `posting_`, `closed_` | `ibv_post_send` and `Account` run outside it |
| `CqCallbackMeta::outcomeMu` | Recorded outcomes | |
| `TransferStatus::msgMu` | Code and message, and the condition variable | |
| `MultithreadExecutor::Worker::mu` | Task queue | |
| `XgmiBackend::ipcMutex` (shared) | `localIpcHandles`, `remoteIpcHandles` | |
| `XgmiBackend::sessionCacheMu`, `FabricBackend::sessionCacheMu` | Session caches | Held while the session is created |
| `XgmiBackend::remoteEnginesMu`, `FabricBackend::remoteEnginesMu` | `remoteEngines` | XGMI takes it in `CanHandle` (`IsSameNodeEngine`), so on every route selection that misses the route cache |
| `StreamPool::mutex_`, `EventPool::mutex_` | HIP streams and events | |
| `TransferCompletion::eventMu` | One XGMI or FABRIC completion (each backend has its own `TransferCompletion`) | Held across `hipEventSynchronize` |

Nesting that exists today. A change must not add the reverse of any of these:

- Per-connection build mutex → `connBuildMapMu_`
- Per-connection build mutex → `ControlPlaneServer::mu`
- Per-connection build mutex → `RdmaManager::mu`
- `ControlPlaneServer::mu` → `RdmaManager::mu`
- `NotifManager::mu` → `RdmaManager::mu`
- `XgmiBackend::sessionCacheMu` → `XgmiBackend::remoteEnginesMu`
- `CqCallbackMeta::outcomeMu` → `TransferStatus::msgMu`

One pair is already taken in both orders, which can deadlock. Do not add a
third user without fixing it:

- `XgmiBackend::GetOrCreateSessionCached` holds `sessionCacheMu` and takes
  `ipcMutex` in `GetRemappedAddress`, for memory of another engine.
- `XgmiBackend::DeregisterMemory` holds `ipcMutex` and takes `sessionCacheMu`
  in `InvalidateSessionsForMemory`.

## Ownership and lifetime

| Object | Owner | Raw or shared uses, and what keeps them valid |
|---|---|---|
| `TransferStatus` | The caller | Raw in `CqCallbackMeta::status`, `OfiOpCtx::status`, XGMI/FABRIC `TransferCompletion::status`. The caller must keep it alive until it is terminal. The Python bindings add no keep-alive |
| `CqCallbackMeta` | `shared_ptr`: the submitter and every ledger record of the transfer | Freed when the last record is released |
| Session from `Backend::CreateSession` | Returned raw from `new`; `IOEngine::CreateSession` puts it in `IOEngineSession::backendSess` (`shared_ptr`) | |
| Cached session | `unique_ptr` in the backend's `sessionCache` | `GetOrCreateSessionCached` returns a raw pointer that is used after the lock is dropped. A replaced RDMA session moves to `retiredSessions`. `InvalidateSessionsForMemory` destroys sessions |
| `EpPair` | Copied by value into route tables, runtimes and sessions | Copies share `sqDepth`, `degraded`, `ledger`, `sqAdmission` through `shared_ptr` |
| QP, CQ, completion channel | `application::RdmaDeviceContext` | A failed QP is not destroyed; it stays until the backend is destroyed. `PendingEndpointGuard` destroys an endpoint that was created but not connected |
| `EndpointRuntime` | `shared_ptr` in `RdmaManager::endpointsById_`, `NotifManager::registeredRuntimes_` and the poll snapshot | `notifCtx` is a raw pointer into `notifCtxById_`, a node-based map |
| Notification RECV buffer | `posix_memalign` in `NotifManager::RegisterEndpoint`, kept in `QpNotifContext::buf` | |
| `ExecutorReq` | The caller's stack | `Task::req` is raw. Valid only because `MultithreadExecutor::RdmaBatchReadWrite` waits for every future before it returns |
| `RdmaContext`, device contexts | `RdmaManager` (raw, deleted in its destructor) | The async monitor holds non-owning `ibv_context*` and is destroyed first |
| XGMI and FABRIC `TransferCompletion` | `shared_ptr` captured by both the wait and the progress callback stored in the `TransferStatus` | Lives as long as the status keeps its callbacks. The shared ownership is what makes a wait on one thread and a poll on another safe |
| HIP event | `EventPool` | Taken per transfer, returned by the completion or by each return before the completion is armed |
| Scatter/gather metadata | `hipMalloc` per kernel batch | Freed by the completion's `cleanup` |
| IPC mapping | `XgmiBackend::remoteIpcHandles` | Closed in `~XgmiBackend` |
| Imported fabric region | `FabricBackend::importedRegions` | Unmapped in `DeregisterMemory` and the destructor |
| `OfiOpCtx`, `OfiBatchState` | Raw `new` at submit | Deleted in `CompleteOp` |

Destruction order in `RdmaBackend`: the destructor stops `notif`, `server` and
`executor`; members are then destroyed in reverse declaration order (`executor`,
`server`, `notif`, `rdma`). `RdmaManager::asyncEventMonitor_` is declared last and
is reset first in the destructor, because its thread calls back into the manager.

## Invariants

A change that breaks one of these needs a matching change everywhere the
invariant is relied on, and a test.

- **I1 Terminal exactly once.** After a status is `IN_PROGRESS`, every path ends
  it in `SUCCESS` or an error, once. RDMA claims the pointer with
  `status.exchange(nullptr)`; XGMI/FABRIC use `std::call_once`.
- **I2 Failed means stopped.** An RDMA status reports an error only when none of
  the transfer's WRs can still move data, so the caller may free or reuse its
  buffers. Documented exception: after a QP is cut, WRITE packets already on the
  wire can still land remotely.
- **I3 Credits balance.** `CqCallbackMeta::totalBatchSize` is the credit total:
  merged requests, or WRs when chunking. Each credit is accounted once, by a
  signaled record's CQE, `FailAll`, `ReleaseOrphanedByRecovery`, a cut QP, or
  `FinishSubmission` for credits that were never posted
  (`totalBatchSize - postedCredits`).
- **I4 `wr_id` zones.** Zone A `[0, notifPerQp)` is notification RECV indices,
  zone B is ledger record ids, zone C (bit 63) is notification SEND ids.
  Unsignaled WRs carry `wr_id` 0; record id 0 means "refused".
- **I5 Every unsignaled WR has a signaled WR behind it on the same QP.** The last
  batch of each endpoint is signaled, and so is any batch whose successor would
  not fit. A submission that gives up settles what it posted with
  `SettlePendingWrs`: completion marker, else cut the QP, else park as orphaned
  and mark the endpoint degraded.
- **I6 Send-queue depth balances.** Reserve before posting. Release exactly once:
  through the ledger on a CQE, in `FailAll` or `ReleaseOrphanedByRecovery`, or by
  the submitter for WRs that never reached the wire. A notification SEND reserves
  one slot and releases it on its SEND CQE. Admission stops at
  `maxSqDepth = SQ size - markerSlots`.
- **I7 No record fails while its post is running.** `InsertForPost` holds the
  record in `Posting` state; `FailAll` closes the ledger, then waits until every
  `PostGuard` has committed. A closed ledger refuses all later records.
- **I8 `degraded` meaning.** Set by a partial post without a signaled tail, or by
  a fatal async event. Cleared only by recovery on a ledger that is not closed.
- **I9 Identity is per device.** A QP is (device, qpn); a CQ is (device, pointer).
- **I10 A failed QP is never reused.** `GetLiveEndpoints` drops endpoints whose
  QP `NeedsQpReplacement`. Replacement is paced by `PaceQpReplacement`.
- **I11 Monitor-thread callbacks** do not throw, block or re-enter the engine.
  The verbs event union is copied before the event is acked.
- **I12 Wire structs are append-only.** `MSGPACK_DEFINE` lists only grow at the end.
- **I13 A bad control-plane message drops that connection only.**
- **I14 HIP thread state is restored.** `ScopedHipDeviceGuard` restores the
  current device; a tolerated HIP error is cleared with `hipGetLastError`.
- **I15 XGMI/FABRIC copies run on backend streams** that are not ordered after
  the caller's streams.
- **I16 Workers assume one device per side.** Worker posting passes
  `localMrPerEp.front()` and `remoteMrPerEp.front()` for every endpoint.
  `UsesInlineOnly` turns workers off only when `numNicsPerTransfer > 1`. A
  session over CPU memory can still get endpoints on several NICs with
  `numNicsPerTransfer == 1`, because `RdmaManager::Search` picks a NIC
  round-robin for each QP that `CreateSession` builds.

## How a transfer uses several QPs

- A session holds `qpPerTransfer` endpoints in `eps`, with `localMrPerEp[i]` and
  `remoteMrPerEp[i]` for `eps[i]`. The lkey and rkey are filled per batch from the
  endpoint that batch goes to.
- Inline posting: batch `i` goes to endpoint `(i + id % epNum) % epNum`. The batch
  size defaults to `ceil(wrCount / epNum)` and is capped by the smallest
  `maxSqDepth`. The WR count can be smaller than the endpoint count, so an
  endpoint can get no work.
- Worker posting: the request list is split into at most
  `min(epNum, numWorker)` ranges; range `i` goes to endpoint
  `(i + id % epNum) % epNum` and runs on worker `epId % numWorker`. Each worker
  calls `RdmaBatchReadWrite` with one endpoint and `ownsTotalBatchSize = false`.
- Bounds and `max_msg_sz` are checked against the first endpoint's MR and the
  smallest endpoint limit; `maxSge` is taken from the first endpoint.
- With notifications on, one SEND goes to every endpoint; the receiver counts
  `totalNum` messages for each transfer id.
- Failure of one QP does not end the transfer until the WRs on the other QPs
  have completed or been flushed (I2, I3).
- Which NIC a QP lands on (`RdmaManager::Search`): for GPU memory, the NIC
  matched to that GPU; for CPU memory, `MORI_IO_RDMA_NIC_IDX` if set, else
  round-robin over the active devices for each QP, or NUMA-local NICs when
  several are requested. So the sessions of one process, and the QPs of one
  CPU-memory session, can sit on different devices with different firmware.

## XGMI and FABRIC

- A session caches the local address, the remote address remapped into this
  process (`GetRemappedAddress`, `GetImportedAddress`) and device ids. It holds
  no sizes.
- A submit takes a stream (round-robin, shared) and an event from the pools,
  issues the copy, records the event, sets `IN_PROGRESS`, and installs wait and
  progress callbacks on the status (`ArmTransferCompletion`). Nothing completes
  until the caller polls or waits.
- IPC handles are exported with device 0 current, and cached per
  (remote engine, memory id, local device).
- `hipSetDevice` changes thread state, so every entry point holds a
  `ScopedHipDeviceGuard`. `StreamPool::GetNextStream` and `EventPool::GetEvent`
  call `hipSetDevice` themselves when they create streams or events, so a
  caller of either needs the guard too.
- In XGMI, an error returned at submit does not stop copies that were already
  queued on the stream (for example when `hipEventRecord` fails after the copy
  was issued). The "failed means stopped" guarantee (I2) is an RDMA guarantee.

## How a status is waited on

`TransferStatus` is in `include/mori/io/common.hpp`.

- `Code`, `InProgress`, `Succeeded`, `Failed`: run the progress callback first,
  but only while the code is `IN_PROGRESS`.
- `WaitFor`: returns at once when the code is not `IN_PROGRESS`. Otherwise it
  polls once, then:
  - `timeoutMs == 0`: returns.
  - `timeoutMs < 0`: with a wait callback, runs it and returns the code, with
    no further wait; without one, waits on the condition variable.
  - `timeoutMs > 0`: waits on the condition variable until the deadline and
    polls between waits. It never runs the wait callback.
- `Update` does nothing when the code is already an error. `SetCode` overwrites
  any code. Both wake waiters.
- The callbacks are never cleared, also after the transfer ends.
- `IOEngine::WaitAll` waits on each status in turn, and an error wins over
  `IN_PROGRESS`.

## Tests that pin the invariants

A change to one of these behaviors changes the test too, with the reason.

| Behavior | Test |
|---|---|
| I2, I3: a failure waits for every QP | `transfer_failure_waits_for_every_qp` in `tests/cpp/io/test_engine.cpp` |
| I4: `wr_id` zones, record id 0 | `wr_id_namespace_helpers`, `submission_ledger_notif_disabled` |
| I6: admission, release wakes a waiter | `sq_admission_release_wakes_waiter`, `sq_admission_degraded_wakes_waiter`, `sq_admission_negative_depth_reserve_repairs_counter` |
| I7: `FailAll` against a running post, closed ledger | `submission_ledger_post_commit_race`, `submission_ledger_close_race_stress`, `submission_ledger_closed_after_fail_all` |
| I9: event scope uses device and number | `async_event_scope_matching` |
| I10: a failed QP is replaced, with pacing | `test_new_session_replaces_failed_qp`, `test_failed_qp_replacements_back_off` in `tests/python/io/test_engine.py` |
| Chunk planning and credits by WR count | `plan_chunks_boundaries`, `chunk_count_split_invariant`, `sge_stream_chunking_covers_and_respects_limits` |
| Polled completion (XGMI), bounded wait | `xgmi_concurrent_wait_and_poll_is_safe`, `xgmi_bounded_wait_returns_on_completion`, `tests/cpp/io/test_transfer_wait.cpp` |
| Peer liveness and event queue | `tests/cpp/io/test_peer_failure.cpp` |
| Control messages: length limit, decode errors | `tests/cpp/io/test_protocol.cpp` |
| Behavior under NIC faults: ends, honest, recovers, alive, stopped, waits | `tests/cpp/io/fault_catalog/ibverbs.yaml`, `async_events.yaml`, run by `test_fault_injection` |

Unit tests build `EpPair`, `ibv_qp` and `ibv_context` objects by hand, with no
ledger and with null `device` and ops. New code that reads verbs structs
directly, outside `Verbs()`, can crash those fixtures.
