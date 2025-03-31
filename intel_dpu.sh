#!/bin/bash
export BUDDY_DPU=${BUDDY_DPU/bf/intel}
exec $*
