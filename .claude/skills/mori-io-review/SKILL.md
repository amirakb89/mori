---
name: mori-io-review
description: >-
  Review MORI-IO changes (src/io, include/mori/io, python/mori/io,
  src/pybind/pybind_io.cpp, tests/cpp/io, tests/python/io) for memory and
  resource leaks, pointer and lifetime errors, unhandled edge cases, multi-QP
  (qpPerTransfer > 1) correctness and multithreading races, plus the
  transfer-status contract, wire compatibility and test quality. Carries the
  thread, lock, ownership and invariant map of the RDMA, XGMI, FABRIC and OFI
  backends. Use when reviewing or writing a pull request, diff or patch that
  touches MORI-IO, when asked to audit MORI-IO code for leaks, races, dangling
  pointers or hangs, or when debugging a transfer that stays IN_PROGRESS, a QP
  failure that is not recovered, or send-queue depth or credits that do not
  balance.
---

# MORI-IO code review

A review under this skill is advisory. It reports what was verified in the
code, and says so when something was not checked or not run.

MORI-IO moves bytes between registered buffers with one-sided operations that
complete on another thread. Its bugs are rarely on the changed line: they are a
path that never ends a transfer, a buffer freed while a WR can still write to
it, a counter that drifts on one error path, or two threads that both think
they own a completion. So every review runs the same six passes.

## Procedure

1. **Get the change.** `git diff <base>...<head>` or `gh pr diff <n>`, for every
   file in it: the MORI-IO paths (`src/io`, `include/mori/io`,
   `python/mori/io`, `src/pybind/pybind_io.cpp`, `tests/cpp/io`,
   `tests/python/io`), `docs/MORI-IO-GUIDE.md`, and the layer below
   (`src/application/transport/rdma/`). List each claim the PR description and
   the new comments make; you check them in step 3.
2. **Read the code, not the diff.** For each changed function, read the whole
   function on head, its callers, and what it calls on the changed paths. Read
   the parts of [architecture.md](architecture.md) for the area:

   | Change touches | Read in architecture.md |
   |---|---|
   | Anything on a transfer path | Entry points and what each validates |
   | `src/io/rdma/common.*`, `ledger.cpp`, `executor.*` | Life of an RDMA transfer, Invariants, How a transfer uses several QPs |
   | `src/io/rdma/backend_impl.*` | Threads, Locks, Ownership and lifetime |
   | `async_event_monitor.*`, `peer_failure*` | Threads, Invariants I9–I11 |
   | `protocol.*`, `MSGPACK_DEFINE` structs | Invariants I12–I13 |
   | `src/io/xgmi`, `src/io/fabric` | XGMI and FABRIC, Invariants I14–I15 |
   | `engine.*`, public headers, Python | Locks, Ownership and lifetime |
   | `TransferStatus` (`include/mori/io/common.hpp`), any completion or wait | How a status is waited on |
   | `src/application/transport/rdma/` | Layers (the layer below) |
   | Behavior that a test may pin | Tests that pin the invariants |

3. **Run the six passes.** All six, every time, in order. A pass with nothing
   to report still gets a row in the report. When the change cannot affect a
   pass, the row says so and names what was read to be sure, for example "adds
   no WR, credit, depth or status state; read `RdmaBatchReadWrite` in full", or
   for pass 4 on an XGMI-only change, "no RDMA code touched; read
   `SelectBackend` to confirm the route of RDMA transfers is unchanged".
4. **Verify each candidate** with [Before you report](#before-you-report).
5. **Write the report** in the [format](#report-format) below.

## The six passes

Rule bodies, with the code each rule is anchored in, are in
[rules.md](rules.md). Read the family for a pass before running it.

### Pass 1: Memory and resources (rules M1–M9)

- List everything the change acquires: heap, HIP memory, handles, events,
  streams and modules, memory regions, QPs, file descriptors, threads,
  address-vector entries, and entries in maps or queues.
- For each one, find the release on the success path and on each early
  `return`, `throw`, `continue` and `break`.
- For each container that gains entries per transfer, memory, peer or QP: what
  erases them, and what bounds the size?
- Shutdown: is it safe to call twice, does it join before it closes or frees,
  and is queued or in-flight work completed or failed?

### Pass 2: Pointers and lifetime (rules P1–P10)

- For each raw pointer, reference or iterator the change adds or newly uses:
  who owns the object, and what keeps it alive at each use, including on other
  threads and after a lock is dropped?
- Null: each pointer from the public API, a lookup, an `optional` or `getenv`.
- `TransferStatus*`: who holds it, and where does each holder let go?
- An object with two holders (two callbacks, a cache and a caller, two
  threads): can one free it while the other still uses it?
- Pointers into vectors and maps against growth and erase. Stack addresses in
  work requests.

### Pass 3: Edge cases (rules E1–E12)

Run these inputs through the changed code and write down what happens:

- An empty batch; one item; all sizes 0; an offset at the end of the buffer.
- A size above `UINT32_MAX`; a merged WR above `max_msg_sz`; `offset + size`
  that wraps.
- Vectors of different lengths; a negative or huge index from the wire or the
  environment. Is the check a runtime check that returns an error, or an
  `assert`?
- Which entry points reach the changed code (`IOEngine`, `IOEngineSession`,
  workers, Python), and does each one pass through the check you rely on?
- Item k of n fails: what happens to items before and after it?
- The call made twice, or out of order; an id that wraps or is reused by a
  restarted peer.
- One GPU, no NIC, one NIC, several NICs, a hidden device, the same device on
  both sides.
- State that is one per process (`static`, `std::once_flag`): what does the
  first user decide for later engines, devices and sessions?

### Pass 4: Multi-QP (rules Q1–Q11)

Read the change again with `qpPerTransfer` 1, 2, and larger than the WR count,
with workers on and off, chunking on and off, notifications on and off.

- Is per-endpoint state (keys, depth, ledger, limits) taken from the endpoint
  the WR goes to?
- Do credits add up, and does the status turn terminal only after every QP's
  WRs are accounted? Show the arithmetic for a count that the QP count does
  not divide.
- Does each QP's unsignaled work have a signaled WR behind it, on every exit?
- Is send-queue depth released exactly once per reserved WR on every path?
- One QP fails mid-transfer: what do the other QPs do, what does the caller
  see, and is that QP replaced on the next transfer?

### Pass 5: Multithreading (rules T1–T13)

- For each changed function, name the threads that can run it and what can run
  at the same time. Always consider: two caller threads on the same session and
  QPs; the CQ poller against a submitter; the async event monitor against both;
  shutdown against all of them.
- For each piece of shared state: which lock, or which atomic, or why it is
  immutable.
- New lock nesting against the order in architecture.md; blocking calls under
  a lock.
- Exactly-once completion; check-then-act; lost wake-ups.

### Pass 6: Transfer status contract (rules S1–S6)

- Follow each `return` and each possible `throw` after the status becomes
  `IN_PROGRESS`. Does every one reach a terminal status, once?
- Is `SUCCESS` reported only when the data landed? Is an error reported only
  when no WR can still move data?

### When the change touches them

| Area | Rules |
|---|---|
| `MSGPACK_DEFINE` structs, `protocol.*`, `NotifMessage` | W1–W4 |
| Environment variables, backend configs, the guide, new comments | C1–C4 |
| `src/io/xgmi`, `src/io/fabric`, any HIP call | X1–X6 |
| Any change in behavior, every test file, every claim in the description | V1–V9 |

## Before you report

Each item is a way to be wrong about this code.

- **The guard may be somewhere else, or missing on another path.** Read head. A
  callee can validate what a caller asserts: on inline posting,
  `RdmaBatchReadWrite` checks at run time the vector lengths that
  `RdmaBackend::BatchReadWrite` only asserts. The reverse also happens: a guard
  in `RdmaBackend::*` does not protect `IOEngineSession` calls, which go
  straight to the session.
- **Introduced or already there?** Compare with the base. Code the change did
  not touch goes under "Pre-existing", not under findings. The "In the tree"
  notes in rules.md list patterns that exist today. An old weakness that the
  change newly reaches, or reaches with new inputs, is a finding against the
  change.
- **Scope the rule to what the change does.** A change that adds a log or a
  diagnostic and touches no WR, credit, depth or status state does not need
  the multi-QP test matrix (V2) or a fault-catalog entry (V4). A cost paid once
  in the process is not a hot-path cost (T9).
- **A leak claim** names the acquisition, the path that skips the release, and
  how often that path runs (per transfer, per peer, per backend).
- **A race claim** names two threads, the shared object and the interleaving.
  **A deadlock claim** names both lock orders.
- **A dangling-pointer claim** names the owner, the free and the later use.
- **Credit and depth claims** show the numbers.
- **By design, not findings:**
  - Unsignaled WRs carry `wr_id` 0.
  - `ibv_post_send` runs outside the ledger mutex; the `PostGuard` covers it.
  - `RdmaNotifyTransfer` points an SGE at a stack `NotifMessage`; the SEND is inline.
  - A failed QP stays allocated, and a replaced session stays in `retiredSessions`.
  - An RDMA failure is reported late, after the transfer's WRs on other QPs end.
  - An XGMI or FABRIC status completes only when the caller polls or waits.
  - Send-queue admission uses `seq_cst`.
  - The thread-local scratch pools keep their capacity up to `kPoolHighWater`.
  - `Backend::CreateSession` returns a raw `new`; `IOEngineSession` owns it.
  - `FailAll` blocks until the posts already admitted have committed. The wait
    is a condition-variable wait, so the ledger mutex is free meanwhile.
  - The CQ poller busy-polls in `POLLING` mode.
- **Say how you know.** "By reading" and "ran `<command>`" are different
  claims. Do not report a test or sanitizer run that was not made. The tree
  has no sanitizer build option.

## Report format

```markdown
## MORI-IO review: <PR, commit or range>, reviewed at <sha of the code read>

**Result:** <n> High, <n> Medium, <n> Low. Advisory.
**Read in full:** <functions and files read on head>
**Ran:** <commands and results, or "nothing run">
**Claims in the description:** <each claim: holds, does not hold, or not checked>

### Findings

#### [High] <rule id>: <what goes wrong, in one line>
`<path>:<line>`
- **When:** <threads, QP count, input or call order that triggers it>
- **Evidence:** <what the code does, with the lines>
- **Fix:** <the change to make>

### Pass coverage

| Pass | Examined | Result |
|---|---|---|
| 1 Memory and resources | <acquisitions and containers followed> | <findings or "none"> |
| 2 Pointers and lifetime | <pointers followed> | |
| 3 Edge cases | <inputs and entry points tried> | |
| 4 Multi-QP | <configurations tried> | |
| 5 Multithreading | <thread pairs tried> | |
| 6 Status contract | <exit paths followed> | |
| Other rules applied | <W, C, X, V rules that the change triggers> | |

### Pre-existing, not introduced by this change
### Not checked
```

- The six pass rows are always present.
- One finding for each defect. Put the rule that names the root cause in the
  heading and the other rules and invariants it breaks in Evidence. When no
  rule fits, use the pass name instead of a rule id.
- Pre-existing items take one line each: the place and what is wrong.
- For a PR comment, write one sentence for the problem, one for when it
  happens and one for the fix.

**Severity.**
- High: a hang; wrong or lost data; a use after free; a crash or abort; a data
  race; a leak that grows with each transfer (say how common the triggering
  transfer is); a supported configuration that fails on every transfer.
- Medium: a leak that grows with each peer, memory or backend; a missing check
  that needs caller misuse to trigger; caller-visible thread state that is
  changed and not restored, such as the HIP current device; a test that pins
  the old behavior and was not updated; no test for a changed posting,
  completion or failure path.
- Low: diagnostics, naming, documentation; no test for a change that only adds
  a log or a diagnostic.

## Running the tests

Run what the host allows and report the result. These are the commands CI uses
(`.github/workflows/ci.yml`). RDMA cases skip without an active RDMA device;
XGMI cases need two GPUs.

```bash
IO_FLAGS="-DBUILD_IO=ON -DBUILD_TESTS=ON -DBUILD_EXAMPLES=OFF -DBUILD_PYBINDS=OFF -DBUILD_OPS=OFF -DBUILD_SHMEM=OFF -DBUILD_CCO=OFF -DBUILD_COLLECTIVE=OFF -DBUILD_METRICS=OFF"

# Engine, ledger, admission, chunking, XGMI
cmake -B build_io $IO_FLAGS
cmake --build build_io -j --target test_engine test_transfer_wait
timeout 300 ./build_io/tests/cpp/test_engine
timeout 120 ./build_io/tests/cpp/test_transfer_wait

# Fault injection: the first five need no GPU or NIC
cmake -B build_fault $IO_FLAGS -DENABLE_IO_FAULT_INJECTION=ON
cmake --build build_fault -j --target test_fault_injection test_fault_injector test_flat_yaml
./build_fault/tests/cpp/test_flat_yaml
MORI_IO_LOG_LEVEL=error ./build_fault/tests/cpp/test_fault_injection --check-catalog
MORI_IO_LOG_LEVEL=error MORI_IO_FAULT_CATALOG=async_events.yaml ./build_fault/tests/cpp/test_fault_injection --check-catalog
MORI_IO_LOG_LEVEL=error ./build_fault/tests/cpp/test_fault_injection --check-async-sources
./build_fault/tests/cpp/test_fault_injector
HIP_VISIBLE_DEVICES=0,1 timeout 900 ./build_fault/tests/cpp/test_fault_injection

# Python, after `pip install .`
PYTHONPATH=$PWD pytest tests/python/io/ -v
MORI_IO_XGMI_SCATTER_GATHER_THRESHOLD=4 PYTHONPATH=$PWD \
  pytest tests/python/io/test_discrete_buffer.py -v -k 'not performance'
```

No workflow runs `test_protocol`, `test_peer_failure`, `test_rail_affinity` or
the C++ `test_discrete_buffer`, and they have no `add_test` entry. Build that
target and run it when it covers the change.

## Reference files

- [architecture.md](architecture.md): layers, the life of a transfer, threads,
  locks and their order, ownership, invariants `I1`–`I16`.
- [rules.md](rules.md): rule bodies `M`, `P`, `E`, `Q`, `T`, `S`, `W`, `C`,
  `X`, `V`, each with the code it is anchored in.
