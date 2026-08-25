#!/bin/bash
# Build + run the ionic IBGDA queue-creation probe inside the moridbg container.
# Adjust NAME / include paths for your environment.
NAME=${1:-moridbg}
docker exec "$NAME" bash -lc '
  cp /host_home/mori/tests/ionic_ibgda/ibgda_qp_create_test.cpp /work/ 2>/dev/null || \
  cp /host_home/ibgda_qp_create_test.cpp /work/
  cd /work
  echo "=== compiling ==="
  hipcc -std=c++17 ibgda_qp_create_test.cpp -o ibgda_qp_create_test \
     -I/work/mori/include -I/opt/rocm/include \
     -libverbs -lionic -ldl -L/usr/lib/x86_64-linux-gnu 2>&1 | tail -20
  echo "=== run: default (GPU-VRAM rings, CCQE on) ==="
  ./ibgda_qp_create_test 2>&1 | grep -vE "libibverbs: Warning"
  echo "RC=${PIPESTATUS[0]}"
  echo
  echo "=== run: MORI_IONIC_HOST_CTRL_BUF=1 (host-pinned rings) ==="
  MORI_IONIC_HOST_CTRL_BUF=1 ./ibgda_qp_create_test 2>&1 | grep -vE "libibverbs: Warning"
  echo "RC=${PIPESTATUS[0]}"
'
