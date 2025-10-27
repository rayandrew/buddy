#!/usr/bin/env python3

import argparse
import uuid
import pandas as pd
import os
import subprocess
import re
import time
import itertools

app_cmds = {
    'mini_histo':   'test/histo 10000 100000 1',
    'histo':        'test/histo 19660800 100000000 1',
    'triangle':     'test/bale/triangle -n 1787345',
    'transpose':    'test/bale/transpose_matrix -n 1787345',
    'sssp':         'test/bale/sssp -n 1787345',
    'qs':           'apps/qs/src/qs -i apps/qs/Examples/CTS2_Benchmark/CTS2-N5.inp -X 64 -Y 64 -Z 32 -x 64 -y 64 -z 32 -I 4 -J 4 -K 2 -n 1310720'
}

def match_pat(s):
    pat = re.compile(s)

    def match_log(log):
        for line in log:
            if mat := re.search(pat, line):
                return mat.group(1)

    return match_log

def qs_time(log):
    us = match_pat('^cycleTracking\s+\S+\s+\S+\s+(\S+)')(log)
    return str(float(us)*1e-6)

app_time_extractor = {
    'mini_histo':   match_pat('^time:\s+([\d\.]+)'),
    'histo':        match_pat('^time:\s+([\d\.]+)'),
    'triangle':     match_pat('^\s+Buddy:\s+([\d\.]+)'),
    'transpose':    match_pat('^\s+Buddy:\s+([\d\.]+)'),
    'sssp':         match_pat('^Bellman-Ford Buddy:\s+([\d\.]+)'),
    'qs':           qs_time,
}

children = []

def run_trial(config, path, dry_run):
    htk_path = os.path.join(path, 'profile')

    if config['offload'] == 'none':
        hosts = 'intel01,intel02'
        numactl = 'numactl -N0'
        build = 'host-rel'
        app_pre = 'scripts/hostname_dpu.sh'
    elif config['offload'] == 'local':
        hosts = 'intel01,intel02'
        numactl = 'numactl -N1'
        build = 'host-rel'
        app_pre = 'scripts/hostname_dpu.sh'
    elif config['offload'] == 'dpu':
        hosts = 'bf01,bf02'
        numactl = ''
        build = 'dpu-rel'
        app_pre = ''
    else:
        raise Exception(f'unknown offload "{config["offload"]}"')

    proxy_cmd = (f'mpirun -np 2 -H {hosts} '
        f'-bind-to none env '
        f'OMP_NUM_THREADS={config["threads"]} BUDDY_QUIET_TIME={config["quiet_time"]} '
        f'{numactl} {build}/src/buddy-proxy')

    app_cmd = (f'mpirun -np 32 -H intel01:16,intel02:16 '
        f'env BUDDY_SENDBUF={config["send_bufs"]} BUDDY_RECVBUF={config["recv_bufs"]} '
        f'{app_pre} numactl -N0 '
        f'$(which hpcrun) -o {htk_path} -ds -e instructions -e LLC-loads -e LLC-load-misses -e BLOCKTIME -e CPUTIME '
        f'host-htk/{app_cmds[config["app"]]}')

    if dry_run:
        print(proxy_cmd)
        print(app_cmd)
    else:
        print(path)

        proxy_log = open(os.path.join(path, 'proxy.log'), 'w')
        app_log = open(os.path.join(path, 'app.log'), 'w')

        print(proxy_cmd)
        proxy_proc = subprocess.Popen(proxy_cmd, shell=True, stdout=proxy_log, stderr=subprocess.STDOUT)

        children = [proxy_proc]

        time.sleep(0.1)

        print(app_cmd)
        app_proc = subprocess.Popen(app_cmd, shell=True, stdout=app_log, stderr=subprocess.STDOUT)

        children.append(app_proc)

        for i in range(2):
            pid, status = os.wait()
            if not os.WIFEXITED(status):
                raise Exception('child did not exit')
            if os.WEXITSTATUS(status) != 0:
                raise Exception('child exited with nonzero status')

        children = []

        # for tool in ("hpcstruct", "hpcprof"):
        #     log = open(os.path.join(path, f'{tool}.log'), 'w')
        #     cmd = [tool, htk_path]
        #     print(" ".join(cmd))
        #     subprocess.run(cmd, check=True, stderr=subprocess.STDOUT, stdout=log)

def collect_metrics(config, path):
    res = {}

    app_log = open(os.path.join(path, 'app.log'), 'r')
    ex = app_time_extractor[config['app']]
    res['time'] = ex(app_log)

    if res['time'] is None:
        raise Exception('Failed to extract time!')

    msg_metrics = frozenset('_'.join([a, b, c]) for a in ('count', 'bytes', 'avg') for b in ('in', 'out') for c in ('local', 'remote'))
    other_metrics = frozenset([
        'max_blocked_thread', 'quiet_flush_events', 'quiet_flush_bufs',
        'h2d_size', 'd2h_size', 'd2d_size',
    ])
    proxy_metrics = msg_metrics | other_metrics

    for line in open(os.path.join(path, 'proxy.log'), 'r'):
        parts = line.split()
        if parts[0] in proxy_metrics:
            res[parts[0]] = parts[1]

    # Fail if not found
    for metric in proxy_metrics:
        res[metric]

    return res

def main():
    p = argparse.ArgumentParser()

    p.add_argument('output')
    p.add_argument('app')
    p.add_argument('offload')

    p.add_argument('-b', '--bufs', default='1:1')
    p.add_argument('-t', '--threads', default='8')

    p.add_argument('-q', '--quiet-time', default='')

    p.add_argument('-n', '--dry-run', action='store_true')
    p.add_argument('-c', '--collect-only', action='store_true')
    p.add_argument('-u', '--uid')

    p.add_argument('-r', '--repeat', default=1, type=int)

    args = p.parse_args()

    if os.system('which mpirun > /dev/null'):
        raise Exception('Cannot find mpirun')

    if os.system('which hpcrun > /dev/null'):
        raise Exception('Cannot find hpctoolkit')

    out = None

    for i in range(args.repeat):
        try:
            iters = itertools.product(*(arg.split(',') for arg in (args.app, args.bufs, args.threads, args.offload, args.quiet_time)))
            for app, bufs, threads, offload, quiet_time in iters:
                if args.uid is None:
                    uid = uuid.uuid4().hex
                else:
                    uid = args.uid

                send_bufs, recv_bufs = bufs.split(':')

                config = {
                    'uid': uid,
                    'app': app,
                    'offload': offload,
                    'send_bufs': send_bufs,
                    'recv_bufs': recv_bufs,
                    'threads': threads,
                    'quiet_time': float(quiet_time) if quiet_time else None,
                }

                new_run = (not args.dry_run) and (not args.collect_only)
                path = os.path.join('app_results', config['uid'])
                if new_run:
                    os.makedirs(path)

                run_trial(config, path, not new_run)
                res = collect_metrics(config, path)

                trial = pd.DataFrame([config | res])
                print(trial)

                if not args.dry_run:
                    if out is None:
                        out = open(args.output, 'a')

                        if out.tell() == 0:
                            htk_cols = ["binary", "instructions_I", "instructions_E", "LLC-loads_I", "LLC-loads_E", "LLC-load-misses_I", "LLC-load-misses_E", "CPUTIME_E", "CPUTIME_I"]
                            header = trial.columns.to_list() + htk_cols
                            print('\t'.join(header), file=out)

                    trial.to_csv(out, index=False, sep='\t', header=False)
                    out.flush()
        finally:
            for child in children:
                child.kill()

if __name__ == '__main__':
    main()
