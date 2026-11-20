# MORI-IO review rules

Rule bodies for the passes in [SKILL.md](SKILL.md). Invariants `I1`–`I16` and the
thread, lock and ownership tables are in [architecture.md](architecture.md).

**"In the tree" notes** show where today's code already has the pattern a rule is
about. They were found by reading `origin/main` at `202946511`; most were not
reproduced at run time. Use them to recognize the pattern, and re-read the code
before citing one. A note does not mean the change under review introduced it:
report such code under "Pre-existing".

## Contents

- [M: Memory and resources](#m-memory-and-resources)
- [P: Pointers and lifetime](#p-pointers-and-lifetime)
- [E: Edge cases](#e-edge-cases)
- [Q: Multi-QP](#q-multi-qp)
- [T: Multithreading](#t-multithreading)
- [S: Transfer status contract](#s-transfer-status-contract)
- [W: Wire and compatibility](#w-wire-and-compatibility)
- [C: Configuration and environment](#c-configuration-and-environment)
- [X: XGMI, FABRIC and HIP](#x-xgmi-fabric-and-hip)
- [V: Tests](#v-tests)

## M: Memory and resources

**M1. Every acquisition has a release on every exit path.**
List what the change acquires: `new`, `posix_memalign`, `hipMalloc`, VMM
reserve/map/import, `hipIpcOpenMemHandle`, `hipModuleLoad`, HIP streams and
events (`EventPool::GetEvent`), memory regions, QPs and CQs, `epoll_create1`,
`eventfd`, TCP endpoints, libfabric objects, address-vector entries. For each,
follow every `return`, `throw`, `continue` and `break` after it. Prefer a guard
object: `PendingEndpointGuard`, `TcpEndpointGuard`, `SubmissionLedger::PostGuard`,
`AsyncEventAckGuard`, `ScopedHipDeviceGuard`.
- Good shape: `XgmiBackendSession::ReadWrite` returns the event on each return
  that comes before the completion is armed. A new early return for a success
  case needs the same.
- In the tree: `NotifManager::RegisterEndpoint` allocates the RECV buffer with
  `posix_memalign`; nothing frees it or deregisters its MR.

**M2. A container that gains an entry per transfer, memory, peer or QP has a matching erase or a cap.**
For each map, vector, set or queue the change adds to: what inserts, what erases,
what bounds it? Per-transfer entries go when the transfer ends, per-memory
entries at `DeregisterMemory`, per-peer entries at `DeregisterRemoteEngine`.
Otherwise there is a stated cap, as in `PeerFailureTracker::kMaxPending`.
- Good shape: `FabricBackend::DeregisterMemory` unmaps and erases its imports.
- In the tree: `NotifManager::notifPool` gains an entry for each inbound
  transfer and `PopInboundTransferStatus` never erases it.
  `XgmiBackend::remoteIpcHandles` is emptied only in the destructor.
  `RdmaBackend::retiredSessions` and `connBuildMu_` only grow.

**M3. `operator[]` on a map inserts.**
In a lookup, read or erase path use `find`.
- In the tree: `RdmaManager::DeregisterRemoteMemory` (`remotes[ekey]`),
  `NotifManager::PopInboundTransferStatus` (`notifPool[remote]`).

**M4. A file descriptor is closed once, then set to -1, after the thread that uses it has stopped.**
- Good shape: `RdmaAsyncEventMonitor::Shutdown`, `ControlPlaneServer::Shutdown`.
- In the tree: `NotifManager::Shutdown` runs `epfd = close(epfd)` before the
  join, which leaves `epfd == 0`. `~RdmaBackend` calls `Shutdown` and
  `~NotifManager` calls it again, so in `PollCqMode::EVENT` the second call
  closes descriptor 0.

**M5. Shutdown is idempotent and ordered.**
Order: stop flag, wake, join, then release what the thread used. A destructor
that calls `Shutdown()` after an explicit `Shutdown()` does nothing. An object a
thread calls into outlives that thread: check member declaration order, because
destruction runs in reverse (see the comment on `RdmaManager::asyncEventMonitor_`).

**M6. Work that is queued or in flight at shutdown is completed or failed, never dropped.**
Check task queues, ledger records and in-flight OFI operations. A dropped
`std::promise` throws `std::future_error` in the waiter. A dropped ledger record
leaves a status `IN_PROGRESS` for ever.
- In the tree: `MultithreadExecutor::Worker::MainLoop` leaves on `!running`
  without draining `q`.

**M7. A resource that only a completion callback releases leaks when the status is never polled.**
XGMI and FABRIC return the event and free scatter/gather metadata inside the
status's own callbacks (`ArmTransferCompletion`). For a new resource tied to
that cleanup, say in the review what happens when the caller drops the status
without waiting.

**M8. Memory-region registration pairs with deregistration, for each device.**
`RegisterLocalMemory(devId, desc)` pairs with `DeregisterLocalMemory`. A cached
remote MR (`RegisterRemoteMemory`) is dropped when the remote memory or the peer
goes away, or a later transfer uses a stale rkey. Registering a key that is
already present does not overwrite a live MR.

**M9. Thread-local and static caches have a bound.** See `kPoolHighWater` in
`RdmaBatchReadWrite`.

## P: Pointers and lifetime

**P1. `TransferStatus*` belongs to the caller.**
It is valid until the status is terminal, and possibly not one instruction
longer. A holder drops the pointer when it makes the status terminal, as
`CqCallbackMeta::status.exchange(nullptr)` does, and never touches it after
`Update`. For a new holder, name the point where it stops using the pointer on
success, on failure and on shutdown.
- In the tree: `pybind_io.cpp` passes `TransferStatus*` with no keep-alive. A
  Python caller that drops the returned status before completion leaves C++
  with a dangling pointer.

**P2. Null checks at the API boundary and after every lookup.**
`status` and `out` parameters; the session from `CreateSession` or
`GetOrCreateSessionCached` (XGMI returns `nullptr` when a remap fails);
`GetEndpointRuntime`; `GetRdmaDeviceContext`; `ep.ledger`, `ep.sqDepth`,
`ep.degraded`, `ep.sqAdmission` (unit tests build `EpPair` without them);
`ibvHandle.qp`, `cq`, `compCh`; `event.element.qp` and `cq`; `std::getenv`;
`std::optional::value()`. Ask for a check where the pointer can be null in
production. Fields that libibverbs always sets (`qp->context`,
`context->device`) are null only in hand-built test fixtures: note that, do not
demand a check.
- In the tree: `SELECT_BACKEND_AND_RETURN_IF_NONE` accepts a null `status`, and
  the lines after it call `status->Failed()`.

**P3. A session pointer from `GetOrCreateSessionCached` is used after the cache lock is released.**
Code that erases or replaces a cache entry keeps the old session alive (as
`retiredSessions` does) or proves that no transfer can still hold it.
- In the tree: `InvalidateSessionsForMemory` (RDMA, XGMI, FABRIC) destroys
  sessions. `DeregisterMemory` on one thread while another is inside
  `ReadWrite` on that memory frees the session under it.

**P4. Pointers into containers.**
`std::vector` growth moves elements: `wr.next` and `wr.sg_list` point into the
WR pools, so nothing is appended to a pool after the `next` links are set, and
`ResetMergedWorkRequestPointers` runs after any change to `sges`.
`unordered_map` element addresses survive a rehash but not an erase
(`EndpointRuntime::notifCtx`). A reference from `operator[]` is not kept across
an unlock.

**P5. Addresses of stack objects in work requests.**
An SGE points at a stack object only with `IBV_SEND_INLINE`, as
`RdmaNotifyTransfer` does with `NotifMessage`. `ibv_send_wr`, `ibv_sge` and
`bad_wr` on the stack are fine: the post copies them. A buffer the NIC reads
after the post outlives the completion.

**P6. Pointers to another thread's stack.**
`MultithreadExecutor::Task::req` points at the caller's `ExecutorReq`, whose
members are references to the caller's vectors. Any change that lets
`MultithreadExecutor::RdmaBatchReadWrite` return before every future is ready
(early return on the first failure, a timeout, an exception from `fut.get()`)
is a use after return.

**P7. `this` in a thread or a stored callback.**
The object outlives the thread or the callback. The wait and progress callbacks
that XGMI and FABRIC store in a `TransferStatus` hold a completion with raw
`EventPool*` and `TransferStatus*`: the backend outlives every status that has
a pending completion.

**P8. Verbs objects after ack or destroy.**
`ibv_async_event.element.*` is not read after `ibv_ack_async_event`.
`QpErrorEvent::cq` is compared by value and never dereferenced. A new endpoint
can get the address or number of a retired one (`ForgetAsyncEndpoint`).

**P9. Raw `new` names its owner in the same function.**
`CreateSession` hands to `IOEngineSession::backendSess`; `OfiOpCtx` hands to
`CompleteOp`. New code uses `std::make_unique` or `std::make_shared` unless the
pointer passes through a C API. The `new` is not the finding by itself: the
finding is an object with no owner on some path, or with two holders where one
frees it while the other can still use it.

**P10. `MemoryDesc::data` of a remote memory is not a local address.**
It is used as an RDMA remote address, or after `GetRemappedAddress` or a fabric
import. Check every new use of `remote.data`.

## E: Edge cases

**E1. Zero, empty and one.**
Size 0, empty batch, a batch whose sizes are all 0, no endpoint, no device, one
GPU, no NIC, null `data`, `deviceId == -1`. Each ends in a defined status, not a
hang or a crash.
- Good shape: `RdmaBatchReadWrite` returns `SUCCESS` for an empty batch and
  `FinishSubmission` reports it; `XgmiBackendSession::BatchReadWrite` returns
  its event when every segment is empty.
- The two entry points differ: through `IOEngine`, `RdmaBackend::BatchReadWrite`
  answers an empty batch itself and never reaches the session; through
  `IOEngineSession`, the session handles it. Check both.

**E2. Range and overflow.**
Test `offset + size` without wrapping: `offset > len || size > len - offset`.
Check each narrowing: `size_t` to `uint32_t` (SGE length), to `int` (WR count,
credits, batch size, kernel arguments), `uint64_t` to `time_t`.
- In the tree: `RdmaBatchReadWrite` tests
  `(localOffsets[i] + sizes[i]) > baseLocalMr.length`; the sum wraps for a huge
  offset. XGMI and OFI sessions do not compare offsets with the registered size.

**E3. `assert` is not input validation, and a check covers only the entry points that pass through it.**
A check on a value from the caller, the wire, the environment or the device is
a runtime check that ends in `ERR_INVALID_ARGS` (transfer path),
`ProtocolError` (control plane) or an exception (setup). `assert` is for
internal invariants only. In this tree's Release build `src/io` is compiled
without `-DNDEBUG`, so an `assert` on such a value aborts the process; in a
build that defines `NDEBUG` the check is gone and the code runs on with the bad
value. Both are findings. Then list the entry points that reach the code
(table in architecture.md) and confirm each one passes through the check:
`IOEngineSession` calls skip every `Backend::*` function.
- Good shape: the length checks at the top of `RdmaBatchReadWrite`, for inline
  posting.
- In the tree: `IOEngine::BatchRead` and `BatchWrite`, and
  `XgmiBackendSession::BatchReadWrite`, check vector lengths only with `assert`.
  `RdmaManager::GetOrCreateDeviceContext` checks `devId` only with `assert`.
  With workers, `MultithreadExecutor::Worker::MainLoop` slices `localOffsets`
  and `remoteOffsets` by indices taken from `sizes.size()` before any length
  check runs.

**E4. A value from the wire or the environment used as an index or a size.**
Check both bounds before use.
- Good shape: `msg.railId` in `HandleControlPlaneProtocol`;
  `ValidateControlMessageLength`.
- In the tree: `msg.nicRank` is limited only from above before
  `candidates[rank]`. The `devId` of an `AskMemoryRegion` message reaches
  `GetOrCreateDeviceContext` unchecked.

**E5. Failure in the middle.**
For each loop that acquires or posts per item: when item k of n fails, earlier
items are released, settled or completed, and later items get a terminal status.
- Good shape: `RdmaNotifyTransfer` gives back the reservations it did not use;
  `SettlePendingWrs`.
- In the tree: `IOEngine::BatchRead` returns at the first item that has no
  backend and leaves the later statuses in `INIT`.

**E6. Repeated and out-of-order calls.**
Second `CreateBackend`, second `RegisterRemoteEngine`, `DeregisterMemory` of an
unknown id, `Shutdown` twice, a transfer after `DeregisterRemoteEngine`,
`RemoveBackend` while sessions exist, and a peer that restarts with the same
engine key and reuses memory ids (stale remote MR, IPC mapping, session, route).

**E7. Counters that wrap and ids that collide.**
`nextTransferUid` is a 32-bit atomic behind a 64-bit `TransferUniqueId`, and ids
key `notifPool`. `MemoryUniqueId` is 32-bit. A ledger record id never falls in
`wr_id` zone A and never has bit 63 set (I4). QP numbers repeat across devices
(I9). The RECV index reuse in `ProcessOneCqe` has a `FIXME`. Memory ids start
at 0 in every engine: a remote memory id means something only together with
its engine key, and comparing it with a local id is a finding.
- In the tree: `InvalidateSessionsForMemory` matches `remoteMemId` by number
  alone, so deregistering local memory also drops sessions to other engines'
  memory with the same number.

**E8. Device and provider limits.**
`max_send_sge` (ionic: 2), `max_msg_sz`, `max_qp_wr` (ionic: 32767),
`max_cqe >= SQ + RQ`, `max_inline_data` for the notification SEND,
`max_rd_atomic`, `notifPerQp <= RQ`. A new WR shape is checked against the
smallest limit over the session's endpoints.

**E9. Time.**
Each wait has a bound, or a written reason to have none. A bounded wait polls a
status that completes only when polled (`TransferStatus::WaitFor`). Cover
`timeoutMs < 0`, `== 0`, `> 0` and a remaining time that overflows `int`. A
sleep is not synchronization.

**E10. A C++ caller can reuse a `TransferStatus`.**
`SetCode(IN_PROGRESS)` restarts it. `Update` never overwrites an error;
`SetCode` overwrites anything. The wait and progress callbacks that XGMI and
FABRIC install stay installed, so a later transfer on another backend that
reuses the object runs a stale callback in `WaitFor(-1)` and returns while
still `IN_PROGRESS`.

**E11. Topology.**
Same device on both sides; a device hidden from `HIP_VISIBLE_DEVICES`; device
ordinals that differ between processes; GPUs outside an XGMI hive; no P2P; one
NIC; several NICs, also from different vendors or with different firmware; a
GPU with no matching NIC; CPU memory with `numaNode == -1`; XGMI-only fallback
(`kXgmiOnlyFallbackPlaceholderPort`); simulation mode.

**E12. Process-wide and once-only state.**
A `static`, a `std::once_flag` or a value read once from the environment is one
per process, not one per engine, backend, device or session. Ask what the first
user decides for everyone after it: a later engine, another NIC, a second
session, a child after `fork`. A once-only check that looks at the first
caller's endpoints says nothing about later callers on other devices.

## Q: Multi-QP

Re-read the change with `qpPerTransfer` 1, 2, and larger than the WR count;
with workers on and off; with chunking on and off; with notifications on and off.

**Q1. Per-endpoint state is indexed by endpoint.**
lkey, rkey, MR, `sqDepth`, `ledger`, `degraded`, `maxSqDepth`, `maxSge` and
`maxMsgSize` belong to one endpoint. Flag `eps[0]` or `.front()` where
`eps[epId]` is needed. Endpoints of one session can sit on different devices
(multi-NIC, or CPU memory with the round-robin NIC choice in
`RdmaManager::Search`); a key from one device's MR is invalid on another
device's QP.
- In the tree: worker posting passes `localMrPerEp.front()` and
  `remoteMrPerEp.front()` for every endpoint, and workers stay on for a
  CPU-memory session whose QPs were spread over NICs by the round-robin choice
  (I16). Bounds use the first MR and `maxSge` comes from `eps[0]`.

**Q2. The status turns terminal only when the credits of every QP are accounted (I2, I3).**
A new failure path goes through `CqCallbackMeta::Account` with the right credit
count. It does not call `status->Update` while WRs of the transfer can still be
in flight on another QP.

**Q3. Credits add up for every split.**
Credits in ledger records plus credits never posted equal `totalBatchSize`.
Show the arithmetic for: fewer requests than QPs, a count not divisible by the
QP count, one request, all requests merged into one WR, more chunks than QPs,
and workers (`ownsTotalBatchSize == false`: the session sets the total with
`CountChunksForSize`, which must equal what `PlanSgeStreamChunks` produces).

**Q4. Each QP's unsignaled WRs have a signaled WR or a completion marker behind them on that QP (I5),** also when the submission stops early.

**Q5. Send-queue depth is reserved and released per QP, the same amount on every path (I6).**
Walk: the reservation fails; the post fails with k of n WRs posted; the ledger
closes between reservation and insert; no marker can be posted; the QP is cut.
Compare with what `RdmaBatchReadWrite` does on each.

**Q6. One QP fails while the others work.**
The transfer fails once. Its WRs on healthy QPs still complete and release
their depth. The failed QP is marked (`RecordQpFailed`, `cutQps`) so the next
transfer replaces it, and healthy QPs are kept. The reported error is the root
cause, not a flush.

**Q7. QP identity is (device, qpn),** as in `EndpointAffectedByAsyncEvent` and
`PeerFailureTracker::QpKey`. A comparison of qpn alone is a finding.

**Q8. Endpoint choice.**
Post on `GetLiveEndpoints`, not `GetAllEndpoint`. Check the counts when some QPs
have failed, and the "insufficient RDMA endpoints" path. A replacement goes
through `PaceQpReplacement`.

**Q9. Distribution.**
The first endpoint rotates with the transfer id. Check `epNum == 1`,
`epNum > wrCount`, `postBatchSize` of 1, -1 and above `maxSqDepth`, and a
`maxSqDepth` that is not a multiple of `postBatchSize`.

**Q10. Notifications.**
One SEND goes to each endpoint and the receiver counts down from `totalNum`. A
SEND that fails on endpoint i of n does not leave the transfer, or the
receiver's count, hanging.

**Q11. Multi-NIC.**
`numNicsPerTransfer > 1` forces inline posting; GPU memory stays on one NIC
(`ResolveRequestedNics`). `InterleaveEndpointsByLocalDevice` returns exactly
`qpPerTransfer` endpoints. With rail affinity both ends use the same
`availDevices` index.

## T: Multithreading

**T1. Name the threads.**
For each function the change touches, list which threads from the table in
architecture.md can run it, and what can run at the same time.

**T2. Shared state has one lock, or is atomic, or is fixed after construction.** Say which.
- In the tree: `IOEngine::backends` and `memPool` have no lock.
  `XgmiBackend::GetScatterGatherFunc` loads a module and writes
  `scatterGatherFuncs_` with no lock, and several caller threads can reach it.
  `ControlPlaneServer::eps` is safe only because one thread uses it.

**T3. Lock order, and blocking under a lock.**
Compare new nesting with the list in architecture.md. Flag a new blocking call
under a lock: TCP connect or read, MR registration, `ibv_post_send`, a HIP
call, `cv.wait`, `future.get`, a sleep. Code that wakes waiters
(`TransferStatus::Update`) runs after the lock is released.
- In the tree: `XgmiBackend::sessionCacheMu` and `ipcMutex` are taken in both
  orders (`GetOrCreateSessionCached` against `DeregisterMemory`).

**T4. Completion happens exactly once (I1).**
Code that makes a status terminal first wins a claim (`exchange(nullptr)`,
`std::call_once`). The CQ poller, the async monitor and the submitter can all
reach the same transfer.

**T5. Check, then act.**
A check made before a lock is taken can be stale: `degraded` then
`InsertForPost` (covered by the closed ledger), cache lookup then create
(covered by the second lookup in `GetOrCreateSessionCached`), `running` then
use. For each new check, say what another thread can do before the act.
- In the tree: `XgmiBackend::GetRemappedAddress` looks up the IPC cache under a
  shared lock, opens the handle with no lock, then stores the result. Two
  threads that both miss both open it, and the first mapping is overwritten
  and never closed. `XgmiBackend::CreateSession` reaches it without
  `sessionCacheMu`.

**T6. Atomics.**
`sqDepth`, `degraded` and `SqAdmissionEvent` use `kSqAdmissionOrder`
(`seq_cst`) on purpose, so that a waiter cannot miss a release. A weaker order,
or a load followed by a store, needs a proof.

**T7. Condition variables and futexes.**
Wait with a predicate. Change the predicate under the mutex. No lost wake-up:
see the `waiters` protocol in `NotifySqStateChanged` and `TryReserveSqDepth`,
and `posting_` with `closed_` in the ledger.

**T8. The async monitor thread (I11).**
Code reachable from its callbacks does not throw, wait on the network, take a
lock that a submitter holds across a post, or re-enter the engine. `FailAll`
waits only for posts that are already running.

**T9. The CQ poller.**
One thread polls every CQ of a backend, so one slow step delays every transfer.
Flag a new lock, allocation, system call, or log at INFO or above on the
per-CQE path. A cost paid once in the process, or on the submit path, is not a
per-CQE cost.

**T10. Thread-local state and re-entry.**
The scratch pools in `RdmaBatchReadWrite` belong to the outermost call
(`reentryDepth`). `ScopedIoCallDiagnosticsCapture` and the HIP current device
are per thread: a worker does not inherit them from the caller.

**T11. Thread start and stop.**
Start the thread after the state it reads is built. Stop it with flag, wake and
join before anything it uses is freed or closed (M4, M5).

**T12. Concurrent public calls.**
Transfers from several threads on one engine are supported and are what Python
does with the GIL released. State what the change assumes about
`RegisterMemory`, `DeregisterMemory` and `RegisterRemoteEngine` running during
transfers, and about two sessions that share QPs. A new assumption goes in the
header. Those calls do run during transfers on other memory: the Python
bindings hold the GIL for them and release it for transfers, so code added to
them is reachable while `SelectBackend` and the backends run on other threads.

**T13. Python.**
The GIL is released in `Read`, `Write`, `Batch*`, `Wait*` and `Pop*`. Those
paths touch no Python object. A new binding for a call that can block releases
the GIL.

## S: Transfer status contract

**S1.** After `SetCode(IN_PROGRESS)`, each `return` and each possible `throw`
leads to a terminal status (I1).

**S2.** No exception leaves the transfer path once the status is `IN_PROGRESS`.
- In the tree: `RdmaBackend::ReadWrite` can throw from
  `GetOrCreateSessionCached` when the first connection fails, before the status
  is set. Python turns it into an exception; note any new case.

**S3.** `SUCCESS` means the data landed and, with notifications on, the
notification was posted.

**S4.** An RDMA error means no WR of the transfer can still move data (I2).
XGMI does not give this guarantee: an error at submit leaves copies that were
already queued on the stream. Check which holds for the backend the change
touches. Do not extend the RDMA guarantee to another backend in a comment or a
doc, and do not weaken it for RDMA.

**S5.** A failure uses `Update` with a message that names the operation and the
cause. `SetCode(SUCCESS)` is guarded where another party can have failed the
status first (see `CompleteOp`).

**S6.** Root cause before flush: `Account(..., rootCause)` and
`IoCallDiagnostics` keep one ERROR log for a transfer; flush cascades log at
DEBUG.

## W: Wire and compatibility

**W1.** `MSGPACK_DEFINE` lists grow only at the end (I12): `EngineDesc`,
`MemoryDesc`, `RasDesc`, `TopoKey`, `MessageRegEndpoint`,
`MessageAskMemoryRegion`. A new field has a default that an old peer's message
leaves in place, and `operator==` is updated.

**W2.** `NotifMessage` travels as raw bytes. A layout change breaks peers on
different versions.

**W3.** Every field read from a peer is validated before use (E4): length
(`kMaxControlMessageBytes`), enum value, id, size
(`fetchedMr.length != remote.size`).

**W4.** A decode error becomes `ProtocolError` and drops one connection (I13).
Peer input never reaches `exit`, `abort` or `assert`.
- In the tree: `SYSCALL_RETURN_ZERO` around `PostRecv` in `ProcessOneCqe` ends
  the process (fault catalog `exit-on-post-recv-fail`).

## C: Configuration and environment

**C1.** A new `MORI_IO_*` variable goes through `env::Override` with a parser
that rejects bad input and keeps the config value. It is validated with the
config (`ValidateRdmaTransferConfig`) and documented in
`docs/MORI-IO-GUIDE.md`.
- In the tree: `MORI_IO_RDMA_NIC_IDX` and `MORI_IB_GID_INDEX` use `std::atoi`.

**C2.** No silent clamp: a value the code changes is logged with the old and
the new value.

**C3.** A changed default is called out in the PR description.

**C4.** Documentation and comments the change adds say what the code does.
Compare each new sentence in `docs/MORI-IO-GUIDE.md`, and each new comment that
states a guarantee, with the code path it describes.

## X: XGMI, FABRIC and HIP

**X1.** Each entry point that calls `hipSetDevice`, `StreamPool::GetNextStream`
or `EventPool::GetEvent` holds a `ScopedHipDeviceGuard` (I14). The pools set
the device themselves when they create streams or events, so a caller that
never names `hipSetDevice` can still change the caller's current device.

**X2.** A HIP error that the code tolerates is cleared with `hipGetLastError`.

**X3.** Copies run on backend streams (I15). Code and tests that write a buffer
on another stream synchronize that device before the transfer and before they
read the result.

**X4.** An event from `EventPool` goes back to the pool of the device it came
from, on each error path and in the completion.

**X5.** IPC export runs with device 0 current. The importer adds `ipcOffset`.
Caches are keyed by (engine, memory id, local device).

**X6.** Kernel launches: the segment count is cast to `int`; the metadata buffer
lives until the event completes.

## V: Tests

**V1.** The change has a test that fails without it. Read the assertions:
constant inputs, a result that is not checked, buffers that are equal before
the transfer. Logic that reads sysfs, the environment or a device directly
cannot be tested: ask for the decision to be a pure function with a test.

**V2.** A change to posting, credits, completion or a failure path is tested
with `qp_per_transfer >= 2` and with `num_worker_threads >= 2` (the `multhd`
configurations in `tests/python/io/test_engine.py`), with notifications on and
off, read and write, single and batch. This matrix is for RDMA. A change that
touches none of those paths (a log, a diagnostic, a doc), or only XGMI or
FABRIC, does not need it; an XGMI or FABRIC change is tested with two caller
threads and with wait and poll on the same status.

**V3.** A concurrency change has a test with real threads and a time bound, as
`submission_ledger_close_race_stress` and
`xgmi_concurrent_wait_and_poll_is_safe` do.

**V4.** A new RDMA failure path has a fault-catalog entry
(`tests/cpp/io/fault_catalog/`) or a `test_fault_injector` case. A fix removes
the `known_bug` and `fails` lines of the entry it fixes. A change does not add
`known_bug` to hide a regression. The injector sits behind `Verbs()`, so it
cannot reach XGMI or FABRIC: a new failure path there needs a unit test, or a
seam around the HIP call so that a test can make it fail.

**V5.** A test that needs hardware skips with a reason (`TestSkip`,
`pytest.mark.skipif`). It never passes without running.

**V6.** GPU tests synchronize every device they wrote, before the transfer and
before the comparison (X3).

**V7.** A new C++ test is added to `tests/cpp/CMakeLists.txt` and to the case
table in `main`. Check that CI runs it: `.github/workflows/ci.yml` builds and
runs named targets only, and most MORI-IO test targets have no `add_test`.

**V8.** A change to behavior that a test pins changes that test, and says why.
The table "Tests that pin the invariants" in architecture.md maps behaviors to
tests. Read the test for the code the change touches: a test that asserts the
old behavior and is left alone means the change breaks it or the test does not
run.

**V9.** Each claim in the PR description is checked against the code. A change
presented as faster carries a measurement: the benchmark command, the numbers
before and after, and the hardware (`docs/MORI-IO-BENCHMARK.md` has the
commands). A comment that says a check is redundant, or that callers already
validate, is a claim: find those callers.
