#!/bin/bash
set -euo pipefail

# -------- Parameter matrix (message size in Bytes) --------
messageSizes=(2 4 8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576)

# -------- Runtime environment & host mapping --------
numaNode=0
PROXY_OMP_THREADS=8

logdir="logs/lat-internode-omp${PROXY_OMP_THREADS}"
mkdir -p "$logdir"

PROXY_BIN="/mnt/nfs/andonghu/project/buddy/buildBF/src/buddy-proxy"
LAT_BIN="./buddy-lat"      # Usage: buddy-lat <max_message_size>

# Proxy (DPU) and Compute (Host) machines
PROXY_HOSTS="bf01,bf02"
LAT_HOST1="intel01"
LAT_HOST2="intel02"


# -------- Cleanup & utilities --------
cleanup() {
  if [[ -n "${proxy_pid:-}" ]] && kill -0 "$proxy_pid" 2>/dev/null; then
    echo "Cleaning up proxy (pid=$proxy_pid) ..."
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

timestamp() { date +"%Y%m%d-%H%M%S"; }

# -------- Fixed to 2 processes (the program requires size == 2) --------
r=2
ts=$(timestamp)
proxy_log="${logdir}/proxy_r${r}_${ts}.log"
lat_log="${logdir}/lat_r${r}_${ts}.log"

{
  echo "======================================================="
  echo "== LAT RUN @ ${ts}  (ranks=${r})"
  echo "== NUMA: ${numaNode}"
  echo "== MATRIX:"
  echo "   messageSizes=${messageSizes[*]}"
  echo "== PROXY: ${PROXY_HOSTS}"
  echo "== PROXY OMP THREADS: ${PROXY_OMP_THREADS}"
  echo "== LAT HOSTS: ${LAT_HOST1}, ${LAT_HOST2}"
  echo "======================================================="
} | tee -a "$proxy_log" >>"$lat_log"

for m in "${messageSizes[@]}"; do
  echo "----> messageSize=${m} B"

  {
    echo
    echo "----- [$(date +'%F %T')] START msg=${m} -----"
  } | tee -a "$proxy_log" >>"$lat_log"

  # Start proxy (both machines)
  echo "[proxy] start for msg=${m}" >>"$proxy_log"
  mpirun --tag-output -np 2 -H "${PROXY_HOSTS}" \
     env OMP_NUM_THREADS="$PROXY_OMP_THREADS" "$PROXY_BIN" >>"$proxy_log" 2>&1 &
  proxy_pid=$!

  # Wait until proxy is ready (adjust if needed)
  sleep 1

  # Run latency: two ranks placed one per host; with NUMA binding
  echo "[lat] run np=${r}, msg=${m}, numa=${numaNode}" >>"$lat_log"
  mpirun --tag-output -np "$r" -H "${LAT_HOST1}:1,${LAT_HOST2}:1" \
    numactl -N "$numaNode" -m "$numaNode" \
    "$LAT_BIN" "$m" >>"$lat_log" 2>&1

  # Wait for proxy to finish up
  if kill -0 "${proxy_pid:-}" 2>/dev/null; then
    wait "$proxy_pid" || true
  fi

  {
    echo "----- [$(date +'%F %T')] END   msg=${m} -----"
    echo
  } | tee -a "$proxy_log" >>"$lat_log"

done

echo "==> Finished LAT sweep"
echo "Logs: proxy -> $proxy_log ; lat -> $lat_log"
echo "-------------------------------------------------------"