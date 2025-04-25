#!/bin/bash

set -Eexo pipefail

: ${ploc?}
: ${pcore?}
: ${omp_bind?}
: ${freq:=3500}
: ${bins:=10000}
: ${load:=10000000}

: ${JDB_CWD:=$PWD}

[ "$freq" = 3500 ]

# build="$JDB_CWD"/host-rel
# dpu_build="$JDB_CWD"/dpu-rel

build="$JDB_CWD"/host-dbg
dpu_build="$JDB_CWD"/dpu-dbg

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

case "$ploc" in
    sock)
        srun -n 2 --ntasks-per-node=1 --label $env numactl -N0 "$build"/src/buddy-proxy > proxy.log &
        mpi_sh=./hostname_dpu.sh
        ;;
    corun)
        srun -n 2 --ntasks-per-node=1 --label $env numactl -N1 "$build"/src/buddy-proxy > proxy.log &
        mpi_sh=./hostname_dpu.sh
        ;;
    *)
        exit 1
        ;;
esac

sleep 1

srun -n 32 --ntasks-per-node=16 numactl -N1 $mpi_sh "$build"/test/histo "$bins" "$load" 1 > histo.log

wait

expect="$(< "$JDB_CWD/test/histo-result/32_${bins}_${load}_1")"
grep -qF "$expect" histo.log
