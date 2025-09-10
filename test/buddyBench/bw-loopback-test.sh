#!/bin/bash
set -euo pipefail

# -------- Parameter matrix --------
ranks=(2 4 8 16)
sizes=(8 16 32 64 128 256)                 # payloadSize
windowSizes=(4 8 16 32 64 128 256)
aggregations=(1 4 16 64 128 256)

# -------- Runtime environment --------
numaNode=0
logdir="logs/bw-loopback"
mkdir -p "$logdir"

# Your binaries/paths (modify if needed)
PROXY_BIN="/mnt/nfs/andonghu/project/buddy-bf/build/src/buddy-proxy"
BW_BIN="./buddy-bw"     # new version: argv = <payloadSize> <windowSize> <aggregation>

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

# -------- Main loop --------
for r in "${ranks[@]}"; do
  echo "===> Running with ranks = $r ..."
  ts=$(timestamp)

  # Generate a pair of log files for each rank count
  proxy_log="${logdir}/proxy_r${r}_${ts}.log"
  bw_log="${logdir}/bw_r${r}_${ts}.log"

  {
    echo "======================================================="
    echo "== RANK $r RUN @ ${ts}"
    echo "== Host: $(hostname)  NUMA: ${numaNode}"
    echo "== MATRIX:"
    echo "   sizes=${sizes[*]}"
    echo "   windowSizes=${windowSizes[*]}"
    echo "   aggregations=${aggregations[*]}"
    echo "======================================================="
  } | tee -a "$proxy_log" >>"$bw_log"

  for s in "${sizes[@]}"; do
    for w in "${windowSizes[@]}"; do
      for a in "${aggregations[@]}"; do
        echo "----> combo: size=${s}, windowSize=${w}, aggregation=${a}"

        {
          echo
          echo "----- [$(date +'%F %T')] START size=${s} window=${w} agg=${a} -----"
        } | tee -a "$proxy_log" >>"$bw_log"

        # Start proxy (background), output appended to proxy_log
        echo "[proxy] start for r=${r}, size=${s}, window=${w}, agg=${a}" >>"$proxy_log"
        mpirun --tag-output -np 1 -H bf01 "$PROXY_BIN" >>"$proxy_log" 2>&1 &
        proxy_pid=$!

        # Wait until proxy is ready (adjust if needed)
        sleep 1

        # Run bandwidth test with NUMA binding
        echo "[bw] run np=${r}, size=${s}, window=${w}, agg=${a}, numa=${numaNode}" >>"$bw_log"
        mpirun --tag-output -np "$r" \
          numactl -N "$numaNode" -m "$numaNode" \
          "$BW_BIN" "$s" "$w" "$a" >>"$bw_log" 2>&1

        # Wait for proxy to exit (if still running)
        if kill -0 "${proxy_pid:-}" 2>/dev/null; then
          wait "$proxy_pid" || true
        fi

        {
          echo "----- [$(date +'%F %T')] END   size=${s} window=${w} agg=${a} -----"
          echo
        } | tee -a "$proxy_log" >>"$bw_log"

      done
    done
  done

  echo "==> Finished ranks = $r"
  echo "Logs: proxy -> $proxy_log ; bw -> $bw_log"
  echo "-------------------------------------------------------"
done