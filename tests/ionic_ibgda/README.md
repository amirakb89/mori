# ionic IBGDA / GPU-initiated RDMA diagnostics

Standalone diagnostics for GPU-initiated RDMA (IBGDA) on the AMD Pensando
ionic RoCE NIC. These isolate the **GPU->NIC doorbell** path (a GPU MMIO
store into the NIC's doorbell BAR) without needing a working CQ/QP or
`amdgpu_peermem`, which is useful on stacks where full IBGDA queue creation
fails at `ionic_dv_create_cq_ex`.

## Tests

- `doorbell_test.cpp` — maps the real `ionic_0` doorbell BAR into GPU address
  space (`hsa_amd_memory_lock_to_pool`, same call as MORI's
  `rocm_memory_lock_to_fine_grain`) and has a GPU kernel do one MMIO store to
  it. Proves the GPU can map + write the NIC BAR without faulting.
  Exit codes: `0` map+store ok, `2` map blocked, `3` GPU MMIO store faulted.

- `doorbell_readback_test.cpp` — maps the same doorbell BAR page two ways
  (CPU-direct via the libionic mmap, and GPU via `lock_to_pool`) and reads the
  first 16 words from both, then compares. Confirms the GPU pointer targets the
  *same physical MMIO page* as the CPU BAR view rather than a silent host-DRAM
  shadow (a non-faulting but NIC-invisible mismap).

## What these do and do NOT prove

- They prove the doorbell BAR is genuinely GPU-visible and writable.
- They do NOT prove end-to-end doorbell *delivery* (that the NIC acts on the
  write) — that needs an armed QP + a peer, i.e. a full IBGDA data-path run.

## Building / running

Requires ROCm (hipcc), rdma-core (`libibverbs`), the ionic userspace provider
(`libionic`), and the MORI headers (for `ionic_dv.h`). The `*_run.sh` scripts
are examples that build and run inside a privileged container that has the
ionic device (`/dev/infiniband`) and GPU (`/dev/kfd`, `/dev/dri`) exposed;
adjust the container name and include paths for your environment.

```sh
hipcc -std=c++17 doorbell_readback_test.cpp -o doorbell_readback_test \
    -I<path-to-mori>/include -I/opt/rocm/include \
    -libverbs -lionic -lhsa-runtime64
./doorbell_readback_test
```
