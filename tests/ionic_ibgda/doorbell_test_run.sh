#!/bin/bash
NAME=moridbg
docker exec "$NAME" bash -lc '
  cp /host_home/doorbell_test.cpp /work/doorbell_test.cpp
  cd /work
  echo "=== compiling ==="
  hipcc -std=c++17 doorbell_test.cpp -o doorbell_test \
     -I/work/mori/include \
     -I/opt/rocm/include \
     -libverbs -lionic -L/usr/lib/x86_64-linux-gnu \
     $(pkg-config --cflags --libs hsa-runtime64 2>/dev/null || echo "-lhsa-runtime64") 2>&1 | tail -20
  echo "=== running (rc meaning: 0=fully works, 2=map blocked, 3=mmio fault) ==="
  ./doorbell_test; echo "TEST_RC=$?"
'
