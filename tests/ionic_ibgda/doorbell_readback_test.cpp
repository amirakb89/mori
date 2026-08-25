// Doorbell BAR read-back sanity test.
//
// Question this answers: when we hsa_amd_memory_lock_to_pool() the ionic
// doorbell BAR page into GPU-visible space (exactly as MORI's
// rocm_memory_lock_to_fine_grain does at ionic.cpp:256-258), does the GPU
// pointer actually target the NIC's MMIO BAR, or could it silently resolve to
// an unrelated host-DRAM shadow page (non-faulting but invisible to the NIC)?
//
// Method: map the SAME physical page two ways -
//   (A) CPU sees it directly as the mmap'd MMIO BAR (dvctx.db_page).
//   (B) GPU sees it via the lock_to_pool handle (gpu_db).
// Then read the first N 64-bit words from BOTH and compare.
//   - GPU words == CPU words (and not all-zero)  -> same physical target: BAR is genuinely GPU-visible.
//   - GPU words != CPU words                      -> GPU is pointed at a DIFFERENT page (DRAM shadow): mapping is a lie.
//   - both all-zero                               -> INCONCLUSIVE (doorbell page may read-as-zero); reported as such.
//
// This proves TARGETING (does the GPU pointer hit the NIC BAR), NOT delivery
// (does the NIC act on a doorbell write). Delivery needs an armed QP + 2 nodes.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <infiniband/verbs.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <hip/hip_runtime.h>
#include "mori/application/transport/rdma/providers/ionic/ionic_dv.h"

#define NWORDS 16

static hsa_agent_t gpu_agent;
static hsa_amd_memory_pool_t cpu_pool;

static hsa_status_t find_gpu(hsa_agent_t agent, void*) {
  hsa_device_type_t t;
  hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &t);
  if (t == HSA_DEVICE_TYPE_GPU) { gpu_agent = agent; return HSA_STATUS_INFO_BREAK; }
  return HSA_STATUS_SUCCESS;
}
static hsa_status_t find_cpu_pool(hsa_amd_memory_pool_t pool, void*) {
  hsa_amd_segment_t seg;
  hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &seg);
  if (seg == HSA_AMD_SEGMENT_GLOBAL) { cpu_pool = pool; return HSA_STATUS_INFO_BREAK; }
  return HSA_STATUS_SUCCESS;
}
static hsa_status_t pick_cpu(hsa_agent_t agent, void*) {
  hsa_device_type_t t;
  hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &t);
  if (t == HSA_DEVICE_TYPE_CPU) hsa_amd_agent_iterate_memory_pools(agent, find_cpu_pool, nullptr);
  return HSA_STATUS_SUCCESS;
}

// GPU kernel: copy NWORDS 64-bit words from the GPU-mapped BAR into an output buffer.
__global__ void gpu_read(volatile uint64_t* bar, uint64_t* out) {
  for (int i = 0; i < NWORDS; i++) out[i] = bar[i];
}

int main() {
  if (hsa_init() != HSA_STATUS_SUCCESS) { printf("HSA init fail\n"); return 1; }
  hsa_iterate_agents(find_gpu, nullptr);
  hsa_iterate_agents(pick_cpu, nullptr);

  int n = 0; ibv_device** list = ibv_get_device_list(&n);
  ibv_context* ctx = nullptr;
  for (int i = 0; i < n; i++) {
    if (strcmp(ibv_get_device_name(list[i]), "ionic_0") == 0) { ctx = ibv_open_device(list[i]); break; }
  }
  if (!ctx) { printf("no ionic_0\n"); return 1; }

  ionic_dv_ctx dvctx; memset(&dvctx, 0, sizeof(dvctx));
  int r = ionic_dv_get_ctx(&dvctx, ctx);
  printf("ionic_dv_get_ctx rc=%d  db_page=%p  db_ptr=%p\n", r, dvctx.db_page, (void*)dvctx.db_ptr);
  if (!dvctx.db_page) { printf("no db_page\n"); return 1; }

  // (A) CPU reads the BAR directly (db_page is the mmap'd MMIO region).
  volatile uint64_t* cpu_bar = reinterpret_cast<volatile uint64_t*>(dvctx.db_page);
  uint64_t cpu_words[NWORDS];
  for (int i = 0; i < NWORDS; i++) cpu_words[i] = cpu_bar[i];

  // Map the same page into GPU space — IDENTICAL call to MORI rocm_memory_lock_to_fine_grain.
  void* gpu_db = nullptr;
  hsa_status_t s = hsa_amd_memory_lock_to_pool(dvctx.db_page, 0x1000, &gpu_agent, 1, cpu_pool, 0, &gpu_db);
  printf("lock_to_pool: status=0x%x  gpu_db=%p\n", s, gpu_db);
  if (s != HSA_STATUS_SUCCESS) { printf("MAP FAILED\n"); return 2; }

  // (B) GPU reads the same NWORDS via the mapped handle.
  uint64_t* dout = nullptr;
  hipMalloc(&dout, NWORDS * sizeof(uint64_t));
  gpu_read<<<1,1>>>(reinterpret_cast<volatile uint64_t*>(gpu_db), dout);
  hipError_t he = hipDeviceSynchronize();
  if (he != hipSuccess) { printf("GPU READ FAULTED: %s\n", hipGetErrorString(he)); return 3; }
  uint64_t gpu_words[NWORDS];
  hipMemcpy(gpu_words, dout, NWORDS * sizeof(uint64_t), hipMemcpyDeviceToHost);

  // Compare.
  int mismatch = 0, cpu_nonzero = 0, gpu_nonzero = 0;
  printf("\n idx |            CPU (BAR)            |            GPU (mapped)\n");
  printf("-----+--------------------------------+--------------------------------\n");
  for (int i = 0; i < NWORDS; i++) {
    if (cpu_words[i]) cpu_nonzero = 1;
    if (gpu_words[i]) gpu_nonzero = 1;
    int diff = (cpu_words[i] != gpu_words[i]);
    if (diff) mismatch++;
    printf(" %3d | 0x%016lx             | 0x%016lx  %s\n", i, cpu_words[i], gpu_words[i], diff ? "<-- DIFF" : "");
  }

  printf("\n=== VERDICT ===\n");
  if (mismatch == 0 && (cpu_nonzero || gpu_nonzero)) {
    printf("MATCH with non-zero content -> GPU pointer targets the SAME physical page as the CPU BAR view.\n");
    printf("The NIC doorbell BAR is genuinely GPU-visible (targeting confirmed).\n");
  } else if (mismatch == 0) {
    printf("MATCH but all-zero -> INCONCLUSIVE. Doorbell page reads as zero from both;\n");
    printf("cannot distinguish 'same BAR' from 'two different zero pages'.\n");
  } else {
    printf("MISMATCH (%d/%d words) -> GPU pointer resolves to a DIFFERENT page than the CPU BAR.\n", mismatch, NWORDS);
    printf("lock_to_pool did NOT create a real P2P BAR mapping (DRAM-shadow suspected).\n");
  }
  return 0;
}
