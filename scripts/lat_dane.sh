#!/bin/bash

set -Eexo pipefail

: ${pcore?}
: ${omp_bind?}
: ${ver:=dbg}

: ${JDB_CWD:=$PWD}

build="$JDB_CWD/host-$ver"
dpu_build="$JDB_CWD/dpu-$ver"

env="env OMP_NUM_THREADS=$pcore OMP_PROC_BIND=$omp_bind"

netmask=$(hwloc-calc socket:1 --cof taskset)
compmask=$(hwloc-calc socket:0 --cof taskset)

for ploc in sock corun; do
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

    srun --ntasks-per-node=1 --label --cpu-bind="mask_cpu:$dpumask" test/lat_slurm.py

    echo
    echo

    srun --overlap --ntasks-per-node=1 --label --cpu-bind="mask_cpu:$dpumask" $env "$build"/src/buddy-proxy > "proxy-$SLURM_JOB_ID.log" &

    srun --overlap --ntasks-per-node=1 --cpu-bind="mask_cpu:$compmask" scripts/hostname_dpu.sh "$build"/test/buddy-lat

    wait -n
done

echo "ok!"
