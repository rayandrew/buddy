#!/bin/bash
set -euo pipefail

ranks=(2 4 8 16 32)
sizes=(8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768)

numaNode=0
logdir="logs/bw-internode"
mkdir -p "$logdir"

cleanup() {
  if [[ -n "${proxy_pid:-}" ]] && kill -0 "$proxy_pid" 2>/dev/null; then
    echo "Cleaning up proxy (pid=$proxy_pid) ..."
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

timestamp() { date +"%Y%m%d-%H%M%S"; }

for r in "${ranks[@]}"; do
  echo "Running with rank $r ..."
  ts=$(timestamp)

  proxy_log="${logdir}/proxy_r${r}_${ts}.log"
  bw_log="${logdir}/bw_r${r}_${ts}.log"

  {
    echo "==============================================="
    echo "== RANK $r RUN @ ${ts}"
    echo "==============================================="
  } >>"$proxy_log" >>"$bw_log"

  for s in "${sizes[@]}"; do
    echo "Running with size $s ..."

    {
      echo
      echo "----- [$(date +'%F %T')] START size=${s} -----"
    } >>"$proxy_log" >>"$bw_log"

    echo "[proxy] start for r=${r}, s=${s}" >>"$proxy_log"
    mpirun --tag-output -np 2 -H bf01,bf02 /mnt/nfs/andonghu/project/buddy-bf/build/src/buddy-proxy \
      >>"$proxy_log" 2>&1 &
    proxy_pid=$!

    sleep 1

    echo "[bw] run np=${r}, size=${s}, numa=${numaNode}" >>"$bw_log"
    mpirun --tag-output -np "$r" -H intel01:$((r/2)),intel02:$((r/2)) \
      numactl -N "$numaNode" -m "$numaNode" \
      ./buddy-bw "$s" >>"$bw_log" 2>&1

    if kill -0 "$proxy_pid" 2>/dev/null; then
      wait "$proxy_pid" || true
    fi

    {
      echo "----- [$(date +'%F %T')] END   size=${s} -----"
      echo
    } >>"$proxy_log" >>"$bw_log"

    echo "Finished size $s"
    echo "Logs (r=$r): proxy -> $proxy_log ; bw -> $bw_log"
    echo "---------------------------------"
  done

  echo "Finished rank $r"
  echo "---------------------------------"
done