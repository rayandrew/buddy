#!/bin/bash
set -euo pipefail

# -------- 参数矩阵（消息大小，单位：Byte）--------
messageSizes=(8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536 131072 262144)

# 注意：程序中 MAXLEN = 4 MiB，且需要 messageSize <= MAXLEN - sizeof(buddy::request_head)
# 如需更大消息，可在此数组中继续增加，或调整程序中的 MAXLEN。

# -------- 运行环境与主机映射 --------
numaNode=0
logdir="logs/lat-internode"
mkdir -p "$logdir"

PROXY_BIN="/mnt/nfs/andonghu/project/buddy-bf/build/src/buddy-proxy"
LAT_BIN="./buddy-lat"      # 用法：buddy-lat <max_message_size>

# 代理（DPU）与计算（Host）主机
PROXY_HOSTS="bf01,bf02"
LAT_HOST1="intel01"
LAT_HOST2="intel02"

# -------- 清理 & 工具 --------
cleanup() {
  if [[ -n "${proxy_pid:-}" ]] && kill -0 "$proxy_pid" 2>/dev/null; then
    echo "Cleaning up proxy (pid=$proxy_pid) ..."
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

timestamp() { date +"%Y%m%d-%H%M%S"; }

# -------- 固定为 2 个进程（程序要求 size == 2）--------
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
  echo "== LAT HOSTS: ${LAT_HOST1}, ${LAT_HOST2}"
  echo "======================================================="
} | tee -a "$proxy_log" >>"$lat_log"

for m in "${messageSizes[@]}"; do
  echo "----> messageSize=${m} B"

  {
    echo
    echo "----- [$(date +'%F %T')] START msg=${m} -----"
  } | tee -a "$proxy_log" >>"$lat_log"

  # 启动 proxy（双机）
  echo "[proxy] start for msg=${m}" >>"$proxy_log"
  mpirun --tag-output -np 2 -H "${PROXY_HOSTS}" \
    "$PROXY_BIN" >>"$proxy_log" 2>&1 &
  proxy_pid=$!

  # 等待 proxy ready（按需调整）
  sleep 1

  # 跑延迟：两个 rank 分布在两台 host 各 1 个；NUMA 绑核
  echo "[lat] run np=${r}, msg=${m}, numa=${numaNode}" >>"$lat_log"
  mpirun --tag-output -np "$r" -H "${LAT_HOST1}:1,${LAT_HOST2}:1" \
    numactl -N "$numaNode" -m "$numaNode" \
    "$LAT_BIN" "$m" >>"$lat_log" 2>&1

  # 等待 proxy 收尾
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