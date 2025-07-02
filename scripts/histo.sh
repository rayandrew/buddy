#!/bin/bash

set -Eexo pipefail

: ${ploc?}
: ${pcore?}
: ${omp_bind?}
: ${freq:=3500}
: ${bins:=10000}
: ${load:=10000000}

: ${JDB_CWD:=$PWD}

build="$JDB_CWD"/host-rel
dpu_build="$JDB_CWD"/dpu-rel

#build="$JDB_CWD"/host-dbg
#dpu_build="$JDB_CWD"/dpu-dbg

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

if which mpirun 2>&1 1>/dev/null; then
    nrun="mpirun -np"
else
    nrun="srun -n"
fi

case "$ploc" in
    sock)
        $env numactl -N0 "$build"/src/buddy-proxy > proxy.log &
        export BUDDY_DPU=localhost
        ;;
    corun)
        $env numactl -N1 "$build"/src/buddy-proxy > proxy.log &
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

$nrun 16 numactl -N1 env BUDDY_DPU=$BUDDY_DPU "$build"/test/histo "$bins" "$load" 1 > histo.log

wait

if [ "$freq" != 3500 ]; then
    sudo cpupower -c "$node1" frequency-set -u 3500MHz
fi

expect="$(< "$JDB_CWD/test/histo-result/16_${bins}_${load}_1")"
grep -qF "$expect" histo.log
