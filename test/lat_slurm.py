#!/usr/bin/env python3

import subprocess
import os
import socket
import sys
import time

nodes_proc = subprocess.run(["scontrol", "show", "hostnames", os.environ["SLURM_JOB_NODELIST"]], capture_output=True, text=True)
nodes = nodes_proc.stdout.split()
me = socket.gethostname()

assert len(nodes) == 2

args = ["-s", "1", "-n", "10000"]

server = me == nodes[0]
if not server:
    args.append(nodes[0])

for met in ("send", "read", "write"):
    if not server:
        time.sleep(1)

    cmd = [f"ib_{met}_lat"] + args
    print("+", cmd, file=sys.stderr)
    subprocess.run(cmd)
