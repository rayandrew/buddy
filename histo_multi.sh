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

build="$JDB_CWD"/host-rel
dpu_build="$JDB_CWD"/dpu-rel

# build="$JDB_CWD"/host-dbg
# dpu_build="$JDB_CWD"/dpu-dbg

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

case "$ploc" in
    sock)
        mpirun -H intel01,intel02 -tag-output $env numactl -N0 "$build"/src/buddy-proxy > proxy.log &
        mpi_sh=./intel_dpu.sh
        ;;
    corun)
        mpirun -H intel01,intel02 -tag-output $env numactl -N1 "$build"/src/buddy-proxy > proxy.log &
        mpi_sh=./intel_dpu.sh
        ;;
    dpu)
        mpirun -H bf01,bf02 -tag-output $env "$dpu_build"/src/buddy-proxy > proxy.log &
        ;;
    *)
        exit 1
        ;;
esac

sleep 1

mpirun -np 32 -H intel01:16,intel02:16 numactl -N1 $mpi_sh "$build"/test/histo "$bins" "$load" 1 > histo.log

wait

expect="$(< "$JDB_CWD/test/histo-result/32_${bins}_${load}_1")"
grep -qF "$expect" histo.log
