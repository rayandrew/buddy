#!/bin/bash

prefix=$1
shift

perf record -F 100 --call-graph dwarf -o "${prefix}-${SLURM_JOB_ID}_${SLURM_PROCID}.data" $*
