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

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

case "$ploc" in
    # sock)
    #     $env numactl -N0 "$build"/src/buddy-proxy > proxy.log &
    #     export BUDDY_DPU=localhost
    #     ;;
    corun)
        $env "$build"/src/buddy-proxy > proxy.log &
        export BUDDY_DPU=localhost
        ;;
    dpu)
        ssh $BUDDY_DPU $env "$dpu_build"/src/buddy-proxy > proxy.log &
        ;;
    *)
        exit 1
        ;;
esac

mpirun -np 32 "$build"/test/histo "$bins" "$load" 1 > histo.log

wait

expect="$(< "$JDB_CWD/test/histo-result/32_${bins}_${load}_1")"
grep -qF "$expect" histo.log
