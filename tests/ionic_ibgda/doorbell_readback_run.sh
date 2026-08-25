#!/bin/bash
NAME=moridbg
docker exec "$NAME" bash -lc '
  cp /host_home/doorbell_readback_test.cpp /work/doorbell_readback_test.cpp
  cd /work
  echo "=== compiling ==="
  hipcc -std=c++17 doorbell_readback_test.cpp -o doorbell_readback_test \
     -I/work/mori/include \
     -I/opt/rocm/include \
     -libverbs -lionic -L/usr/lib/x86_64-linux-gnu \
     $(pkg-config --cflags --libs hsa-runtime64 2>/dev/null || echo "-lhsa-runtime64") 2>&1 | tail -20
  echo "=== running ==="
  ./doorbell_readback_test; echo "TEST_RC=$?"
'
