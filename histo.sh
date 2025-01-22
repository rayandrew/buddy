#!/bin/bash

set -ex

: ${ploc?}
: ${pcore?}
: ${omp_bind:=1}
: ${freq:=3500}

: ${JDB_CWD:=.}

build="$JDB_CWD"/host-rel
dpu_build="$JDB_CWD"/dpu-rel

env="env OMP_NUM_THREADS=$pcore"
if [ "$omp_bind" != 0 ]; then
    env="$env OMP_PROC_BIND=true"
fi

case "$ploc" in
    sock)
        $env numactl -N0 "$build"/src/buddy-proxy > proxy.log &
        export BUDDY_DPU=localhost
        ;;
    dpu)
        ssh $BUDDY_DPU $env "$dpu_build"/src/buddy-proxy > proxy.log &
        ;;
    *)
        exit 1
        ;;
esac

node1="$(numactl -H | grep -Po "^node 1 cpus: \K.*" | tr " " ,)"

if [ "$freq" != 3500 ]; then
    sudo cpupower -c "$node1" frequency-set -u "$freq"MHz
fi

mpirun -np 16 numactl -N1 "$build"/test/histo 10000 10000000 1 > histo.log

wait

if [ "$freq" != 3500 ]; then
    sudo cpupower -c "$node1" frequency-set -u 3500MHz
fi

grep -qF 'RESULT: 0[5962] = 1137
RESULT: 4[1774] = 1133
RESULT: 11[4778] = 1162
RESULT: 1[4807] = 1131' histo.log
