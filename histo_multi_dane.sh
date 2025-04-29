#!/bin/bash

set -Eexo pipefail

: ${ploc?}
: ${pcore?}
: ${omp_bind?}
: ${freq:=3500}
: ${bins:=10000}
: ${load:=10000000}
: ${node_ranks:=16}

: ${JDB_CWD:=$PWD}

[ "$freq" = 3500 ]

# build="$JDB_CWD"/host-rel
# dpu_build="$JDB_CWD"/dpu-rel

build="$JDB_CWD"/host-dbg
dpu_build="$JDB_CWD"/dpu-dbg

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

netmask=$(hwloc-calc socket:1 --cof taskset)
compmask=$(hwloc-calc socket:0 --cof taskset)

case "$ploc" in
    sock)
        srun --overlap --ntasks-per-node=1 --label --cpu-bind="mask_cpu:$netmask" $env "$build"/src/buddy-proxy > "proxy-$SLURM_JOB_ID.log" &
        mpi_sh=./hostname_dpu.sh
        ;;
    corun)
        srun --overlap --ntasks-per-node=1 --label --cpu-bind="mask_cpu:$compmask" $env "$build"/src/buddy-proxy > "proxy-$SLURM_JOB_ID.log" &
        mpi_sh=./hostname_dpu.sh
        ;;
    *)
        exit 1
        ;;
esac

sleep 1

srun --overlap --ntasks-per-node="$node_ranks" --cpu-bind="mask_cpu:$compmask" $mpi_sh "$build"/test/histo "$bins" "$load" 1 > "histo-$SLURM_JOB_ID.log"

wait -n

ranks="$(( $node_ranks * $SLURM_JOB_NUM_NODES ))"
expect="$(< "$JDB_CWD/test/histo-result/${ranks}_${bins}_${load}_1")"
grep -qF "$expect" histo.log

echo "ok!"
