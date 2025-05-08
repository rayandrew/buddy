#!/bin/bash

set -Eexo pipefail

if [ "$SLURM_ARRAY_TASK_ID" ]; then
    pcore="$(($SLURM_ARRAY_TASK_ID / 2 + 1))"
    case "$(($SLURM_ARRAY_TASK_ID % 2))" in
        0) ploc=sock ;;
        1) ploc=corun ;;
        *) exit 1 ;;
    esac
    fileid="${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}"
else
    fileid="$SLURM_JOB_ID"
fi

: ${ploc?}
: ${pcore?}
: ${omp_bind?}
: ${bins:=10000}
: ${load:=10000000}
: ${node_ranks:=16}
: ${ver:=dbg}

: ${JDB_CWD:=$PWD}

build="$JDB_CWD/host-$ver"
dpu_build="$JDB_CWD/dpu-$ver"

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

if [ "$perf" ]; then
    env="$env ./multiperf.sh perf-proxy"
fi

netmask=$(hwloc-calc socket:1 --cof taskset)
compmask=$(hwloc-calc socket:0 --cof taskset)

case "$ploc" in
    sock)
        dpumask="$netmask"
        ;;
    corun)
        dpumask="$compmask"
        ;;
    *)
        exit 1
        ;;
esac

# start proxy
srun --overlap --ntasks-per-node=1 --label --cpu-bind="mask_cpu:$dpumask" $env "$build"/src/buddy-proxy > "proxy-$fileid.log" &

sleep 1

# start app
srun --overlap --ntasks-per-node="$node_ranks" --cpu-bind="mask_cpu:$compmask" ./hostname_dpu.sh "$build"/test/histo "$bins" "$load" 1 > "histo-$fileid.log"

wait -n

ranks="$(( $node_ranks * $SLURM_JOB_NUM_NODES ))"
expect="$(< "$JDB_CWD/test/histo-result/${ranks}_${bins}_${load}_1")"
grep -qF "$expect" histo-$fileid.log

echo "ok!"
