---
applyTo: "src/io/**,include/mori/io/**,python/mori/io/**,src/pybind/pybind_io.cpp,tests/cpp/io/**,tests/python/io/**"
---

# MORI-IO: PR review rules

The full review contract is the in-repo skill `.claude/skills/mori-io-review/`:
`SKILL.md` (procedure and report format), `rules.md` (rules `M`, `P`, `E`, `Q`,
`T`, `S`, `W`, `C`, `X`, `V`) and `architecture.md` (threads, locks, ownership,
invariants `I1`–`I16`). Open and apply them. The rules below are the minimum
for every review and do not replace them.

## Facts about this code

- Transfers are asynchronous. The caller owns the `TransferStatus`; the library
  keeps a raw pointer to it until the status is terminal.
- Three kinds of thread can end one RDMA transfer: the submitting thread, the CQ
  poller (`NotifManager::MainLoop`) and the async event monitor
  (`RdmaAsyncEventMonitor`). Worker threads (`MultithreadExecutor`) post when
  `numWorkerThreads > 1`.
- A transfer is spread over `qpPerTransfer` QPs. Each QP has its own send-queue
  depth counter, submission ledger, `degraded` flag and memory-region keys.
- An RDMA status reports an error only after every WR of the transfer, on every
  QP, has completed or been flushed. The caller may then free its buffers.
- XGMI and FABRIC have no thread: a status completes when the caller polls or
  waits, and the copy runs on a backend stream that is not ordered after the
  caller's streams.

## Check these six in every review

Read the whole changed function on the head branch, and its callers, not only
the changed lines.

1. **Memory and resources.** For each thing the change acquires (heap, HIP
   memory, events, streams, modules, memory regions, QPs, file descriptors,
   threads, map or queue entries), find the release on the success path and on
   every early `return` and `throw`. A map that gains an entry per transfer,
   memory or peer needs a matching erase or a cap; `operator[]` in a lookup
   inserts. Shutdown must be safe to call twice, must join the thread before it
   closes a descriptor or frees memory, and must set a closed descriptor to -1.
2. **Pointers and lifetime.** For each raw pointer or reference: who owns the
   object and what keeps it alive at each use, on every thread, after the lock
   is dropped? Check for null at the public API and after each lookup. A cached
   session pointer is used after the cache lock is released, so code that
   erases a cache entry must not free a session a transfer can still hold.
   Pointers into a `std::vector` do not survive growth. An SGE may point at a
   stack object only with `IBV_SEND_INLINE`.
3. **Edge cases.** Empty batch, size 0, one item, an offset at the end of the
   buffer, a size above `UINT32_MAX`, a WR above `max_msg_sz`, vectors of
   different lengths, a call made twice or out of order, failure at item k of
   n. Test `offset + size` without wrap-around. A value from the caller, the
   wire or the environment needs a runtime check on both bounds that returns
   an error. `assert` is not such a check: in this tree's Release build it
   aborts the process, and under `NDEBUG` it is gone. A check covers only the
   entry points that pass through it: `IOEngineSession` calls go straight to
   the session and skip every check in `IOEngine` and `Backend::*`.
4. **Multi-QP.** Read the change with `qpPerTransfer` 1, 2, and larger than the
   WR count. Per-QP state must come from the QP the WR goes to, not from
   `eps[0]` or `.front()`. Credits must add up when the QP count does not
   divide the work. Each QP's unsignaled WRs need a signaled WR behind them on
   that QP. Send-queue depth is released exactly once for each reserved WR on
   every path. When one QP fails, the transfer fails once, the other QPs finish,
   and the failed QP is replaced. A QP is identified by device and number.
5. **Multithreading.** Name the threads that can run each changed function and
   what runs at the same time: two callers on one session, the CQ poller
   against a submitter, the async monitor against both, shutdown against all.
   For each shared object, name its lock or atomic. Flag a new lock order, a
   blocking call under a lock, a check made before a lock that is used after
   it, and any second way to complete a status.
6. **Transfer status.** After a status is `IN_PROGRESS`, every `return` and
   `throw` must lead to `SUCCESS` or an error, exactly once. `SUCCESS` only
   when the data landed. No exception may leave the transfer path.

Also flag: a `MSGPACK_DEFINE` list changed anywhere but at the end; a new
`MORI_IO_*` variable that is not parsed through `env::Override` or not in
`docs/MORI-IO-GUIDE.md`; a HIP entry point without `ScopedHipDeviceGuard`
(taking a stream or an event from the pools also sets the device); a
`static` or `std::once_flag` whose first user decides for every later engine,
device or session; a behavior change without a test that fails before it; a
test that pins the old behavior and is left unchanged; a change to posting,
completion or a failure path that is tested only with one QP and one thread; a
GPU test that does not synchronize the devices it wrote before the transfer.

Check each claim in the PR description and in new comments against the code.
"Callers already validate" needs those callers named. A change presented as
faster needs a benchmark command and numbers from before and after.

## Before you report

- Compare with the base branch. Report a defect the PR did not introduce
  separately and say that it is already there.
- Match the rule to the change. A PR that only adds a log or a diagnostic does
  not need multi-QP tests or a fault-catalog entry.
- A leak comment names the acquisition and the path that skips the release. A
  race comment names two threads and the interleaving. A deadlock comment names
  both lock orders. A dangling-pointer comment names the free and the later use.
- These are by design: unsignaled WRs with `wr_id` 0; `ibv_post_send` outside
  the ledger mutex; a failed QP that stays allocated; an RDMA error that is
  reported late; an XGMI status that completes only when polled; `seq_cst` on
  send-queue admission; the raw `new` in `CreateSession`.

## How to write comments

- One sentence for the problem, one for when it happens (threads, QP count or
  input), one for the fix. Use simple words and short sentences.
- Do not describe what the code does. Do not report style that `pre-commit`
  already enforces.
- Set the severity to High for a hang, lost or wrong data, a use after free, a
  crash, a data race, or a leak that grows with each transfer.
