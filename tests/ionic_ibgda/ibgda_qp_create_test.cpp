// Self-contained ionic IBGDA queue-creation capability probe.
//
// Replicates MORI's exact GPU-initiated (IBGDA) setup path on a SINGLE node,
// with no peer and no amdgpu_peermem, and reports precisely which step
// succeeds or fails. This is the path that fails on dma_buf-only ionic
// fabrics at ionic_dv_create_cq_ex (errno=14 EFAULT).
//
// Steps (mirroring src/.../providers/ionic/ionic.cpp):
//   1. open ionic_0, alloc a normal PD
//   2. alloc a parent domain whose ring allocator is GPU-VRAM
//      (hipExtMallocWithFlags Uncached) -- this is what needs the ring pinned
//      for the NIC; sqcmb/rqcmb off, udma_mask 1<<i
//   3. create the CQ via ionic_dv_create_cq_ex with the CCQE flag over that
//      parent domain  <-- the step that EFAULTs without peermem/dmabuf-queues
//   4. create an RC QP over that CQ + parent domain
//   5. enable GDA mode on the QP (ionic_dv_qp_set_gda)
//   6. fetch GPU-side ring/doorbell handles (get_cq / get_qp / get_ctx)
//
// Exit code = the number of the first step that FAILED (0 = all passed).
//
// Env toggles (match MORI):
//   MORI_IONIC_HOST_CTRL_BUF=1  -> allocate rings in coherent host memory
//                                  (hipHostMalloc) instead of GPU VRAM. Clears
//                                  the create EFAULT where peermem is absent.
//   MORI_DISABLE_IONIC_CCQE=1   -> build the CQ without the CCQE flag.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <dlfcn.h>
#include <infiniband/verbs.h>
#include <hip/hip_runtime.h>
#include "mori/application/transport/rdma/providers/ionic/ionic_dv.h"

static bool env_on(const char* k) {
  const char* v = std::getenv(k);
  return v && std::strcmp(v, "1") == 0;
}

#define STEP(n, msg) do { printf("[step %d] %s ... ", n, msg); fflush(stdout); } while (0)
#define OK()         do { printf("OK\n"); } while (0)
#define FAIL(n, ...) do { printf("FAILED (errno=%d %s): ", errno, strerror(errno)); \
                          printf(__VA_ARGS__); printf("\n"); return n; } while (0)

// Parent-domain ring allocator: GPU VRAM by default, coherent host mem under
// MORI_IONIC_HOST_CTRL_BUF=1. Mirrors IonicDeviceContext::pd_alloc_device_uncached.
static void* pd_alloc(struct ibv_pd*, void*, size_t size, size_t, uint64_t) {
  void* p = nullptr;
  if (env_on("MORI_IONIC_HOST_CTRL_BUF")) {
    if (hipHostMalloc(&p, size, hipHostMallocCoherent) != hipSuccess) return nullptr;
  } else {
    if (hipExtMallocWithFlags(&p, size, hipDeviceMallocUncached) != hipSuccess) return nullptr;
  }
  memset(p, 0, size);
  return p;
}
static void pd_free(struct ibv_pd*, void*, void* ptr, uint64_t) {
  if (!ptr) return;
  if (env_on("MORI_IONIC_HOST_CTRL_BUF")) hipHostFree(ptr); else hipFree(ptr);
}

int main() {
  const bool host_ctrl = env_on("MORI_IONIC_HOST_CTRL_BUF");
  const bool ccqe = !env_on("MORI_DISABLE_IONIC_CCQE");
  printf("=== ionic IBGDA queue-creation probe ===\n");
  printf("ring memory : %s\n", host_ctrl ? "coherent HOST (hipHostMalloc)" : "GPU VRAM (hipExtMallocWithFlags Uncached)");
  printf("CCQE        : %s\n\n", ccqe ? "enabled" : "disabled");

  // ---- step 1: open device + PD ----
  STEP(1, "open ionic_0 + alloc PD");
  int n = 0; ibv_device** list = ibv_get_device_list(&n);
  ibv_context* ctx = nullptr;
  for (int i = 0; i < n; i++)
    if (!strcmp(ibv_get_device_name(list[i]), "ionic_0")) { ctx = ibv_open_device(list[i]); break; }
  if (!ctx) FAIL(1, "could not open ionic_0");
  ibv_pd* pd_orig = ibv_alloc_pd(ctx);
  if (!pd_orig) FAIL(1, "ibv_alloc_pd failed");
  OK();

  // ---- step 2: parent domain with GPU-VRAM ring allocator ----
  STEP(2, "alloc parent domain (GPU-VRAM ring allocator)");
  struct ibv_parent_domain_init_attr pattr;
  memset(&pattr, 0, sizeof(pattr));
  pattr.pd = pd_orig;
  pattr.comp_mask = IBV_PARENT_DOMAIN_INIT_ATTR_ALLOCATORS;
  pattr.alloc = pd_alloc;
  pattr.free = pd_free;
  ibv_pd* pd_ux[2] = {nullptr, nullptr};
  for (int i = 0; i < 2; i++) {
    errno = 0;
    pd_ux[i] = ibv_alloc_parent_domain(ctx, &pattr);
    if (!pd_ux[i]) FAIL(2, "ibv_alloc_parent_domain[%d] failed", i);
    ionic_dv_pd_set_sqcmb(pd_ux[i], false, false, false);
    ionic_dv_pd_set_rqcmb(pd_ux[i], false, false, false);
    ionic_dv_pd_set_udma_mask(pd_ux[i], 1u << i);
  }
  OK();

  // ---- step 3: CQ via ionic_dv_create_cq_ex (+CCQE) over the parent domain ----
  STEP(3, "ionic_dv_create_cq_ex over parent domain");
  struct ibv_cq_init_attr_ex cq_attr;
  memset(&cq_attr, 0, sizeof(cq_attr));
  cq_attr.cqe = 1024;
  cq_attr.comp_mask = IBV_CQ_INIT_ATTR_MASK_PD;
  cq_attr.parent_domain = pd_ux[0];
  struct ionic_cq_init_attr_ex ion_cq;
  memset(&ion_cq, 0, sizeof(ion_cq));
  if (ccqe) {
    ion_cq.comp_mask = IONIC_CQ_INIT_ATTR_MASK_FLAGS;
    ion_cq.flags = IONIC_CQ_INIT_ATTR_CCQE;
  }
  errno = 0;
  struct ibv_cq_ex* cq_ex = ionic_dv_create_cq_ex(ctx, &cq_attr, &ion_cq);
  if (!cq_ex)
    FAIL(3, "this is the IBGDA wall: GPU-resident CQ ring could not be pinned for the NIC");
  ibv_cq* cq = ibv_cq_ex_to_cq(cq_ex);
  OK();

  // ---- step 4: RC QP over that CQ + parent domain ----
  STEP(4, "create RC QP over parent domain");
  struct ibv_qp_init_attr_ex qa;
  memset(&qa, 0, sizeof(qa));
  qa.qp_type = IBV_QPT_RC;
  qa.send_cq = cq; qa.recv_cq = cq;
  qa.cap.max_send_wr = 256; qa.cap.max_recv_wr = 256;
  qa.cap.max_send_sge = 1; qa.cap.max_recv_sge = 1;
  qa.comp_mask = IBV_QP_INIT_ATTR_PD;
  qa.pd = pd_ux[0];
  errno = 0;
  ibv_qp* qp = ibv_create_qp_ex(ctx, &qa);
  if (!qp) FAIL(4, "ibv_create_qp_ex failed");
  OK();

  // ---- step 5: enable GDA mode ----
  STEP(5, "ionic_dv_qp_set_gda(enable_send, enable_recv)");
  errno = 0;
  int r = ionic_dv_qp_set_gda(qp, true, true);
  if (r != 0) FAIL(5, "ionic_dv_qp_set_gda returned %d", r);
  OK();

  // ---- step 6: fetch GPU-side ring + doorbell handles ----
  STEP(6, "ionic_dv_get_ctx (GPU doorbell BAR handle)");
  ionic_dv_ctx dvctx; memset(&dvctx, 0, sizeof(dvctx));
  if (ionic_dv_get_ctx(&dvctx, ctx) != 0 || !dvctx.db_page) FAIL(6, "ionic_dv_get_ctx / db_page");
  OK();

  // get_send_dbell_data is declared in ionic_dv.h but may not be exported by
  // the installed libionic; resolve it at runtime so a missing symbol is a
  // reported finding, not a link failure.
  STEP(7, "resolve ionic_dv_qp_get_send_dbell_data (optional)");
  using getdb_fn = int (*)(struct ibv_qp*, uint64_t*);
  getdb_fn getdb = (getdb_fn)dlsym(RTLD_DEFAULT, "ionic_dv_qp_get_send_dbell_data");
  if (!getdb) {
    printf("NOT EXPORTED by libionic (declared in header only)\n");
  } else {
    uint64_t sq_db = 0;
    int rr = getdb(qp, &sq_db);
    if (rr != 0) printf("present but returned %d (errno=%d %s)\n", rr, errno, strerror(errno));
    else printf("OK  sq_dbell_data=0x%lx\n", (unsigned long)sq_db);
  }

  printf("\n=== IBGDA queue setup PASSED through step 6 on this node ===\n");
  printf("db_page=%p\n", dvctx.db_page);
  return 0;
}
