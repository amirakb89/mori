// Isolated GPU->NIC doorbell P2P BAR test.
// Replicates MORI's ionic.cpp doorbell mapping WITHOUT needing peermem or a working CQ/QP:
//   1. open ionic device, ionic_dv_get_ctx -> db_page (NIC doorbell BAR page)
//   2. hsa_amd_memory_lock_to_pool(db_page) -> map NIC BAR into GPU address space
//   3. launch a GPU kernel that does one store to that GPU-visible doorbell pointer
// Success of (2) proves ROCm can P2P-map the NIC BAR; success of (3) proves the GPU can MMIO-write it.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <infiniband/verbs.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <hip/hip_runtime.h>
#include "mori/application/transport/rdma/providers/ionic/ionic_dv.h"

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

__global__ void ring(volatile uint64_t* db) { *db = 0x1ULL; }

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
  printf("ionic_dv_get_ctx rc=%d  db_page=%p\n", r, dvctx.db_page);
  if (!dvctx.db_page) { printf("no db_page\n"); return 1; }

  // THE TEST: map the NIC doorbell BAR into GPU address space.
  void* gpu_db = nullptr;
  hsa_status_t s = hsa_amd_memory_lock_to_pool(dvctx.db_page, 0x1000, &gpu_agent, 1, cpu_pool, 0, &gpu_db);
  printf("hsa_amd_memory_lock_to_pool: status=0x%x  gpu_db=%p\n", s, gpu_db);
  if (s != HSA_STATUS_SUCCESS) { printf("DOORBELL MAP FAILED (P2P BAR blocked)\n"); return 2; }
  printf("DOORBELL MAP OK — NIC BAR is GPU-visible\n");

  // Second half: GPU actually writes the doorbell (MMIO store from a kernel).
  ring<<<1,1>>>((volatile uint64_t*)gpu_db);
  hipError_t he = hipDeviceSynchronize();
  printf("GPU doorbell store: %s\n", he == hipSuccess ? "OK (no fault)" : hipGetErrorString(he));
  printf(he == hipSuccess ? "DOORBELL P2P FULLY WORKS\n" : "GPU MMIO STORE FAULTED\n");
  return he == hipSuccess ? 0 : 3;
}
