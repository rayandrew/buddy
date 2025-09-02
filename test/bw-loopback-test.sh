#!/bin/bash
set -euo pipefail

ranks=(2 4 8 16)
sizes=(8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768)

numaNode=0
logdir="logs/bw-loopback"
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

  # 每个 rank 数量一套日志文件
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

    # 启动 proxy（后台），输出追加到同一个 proxy_log
    echo "[proxy] start for r=${r}, s=${s}" >>"$proxy_log"
    mpirun --tag-output -np 1 -H bf01 /mnt/nfs/andonghu/project/buddy-bf/build/src/buddy-proxy \
      >>"$proxy_log" 2>&1 &
    proxy_pid=$!

    # 给 proxy 一点时间（按需调整）
    sleep 1

    # 运行带宽测试，NUMA 绑定，输出追加到同一个 bw_log
    echo "[bw] run np=${r}, size=${s}, numa=${numaNode}" >>"$bw_log"
    mpirun --tag-output -np "$r" \
      numactl -N "$numaNode" -m "$numaNode" \
      ./buddy-bw "$s" >>"$bw_log" 2>&1

    # 等待 proxy 退出（若仍在）
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