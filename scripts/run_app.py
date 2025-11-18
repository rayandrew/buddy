#!/usr/bin/env python3

import argparse
import uuid
import pandas as pd
import os
import subprocess
import re
import time
import itertools
import random

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

class ChildKilled(Exception): pass
class ChildCrashed(Exception): pass

def run_trial(config, path, dry_run, profile, timeout='0'):
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

    trace_proxy = False
    trace_app = False
    profile_app = False

    match profile:
        case '' | None:
            pass
        case 'app':
            profile_app = True
        case 'trace-proxy':
            trace_proxy = True
        case 'trace_app':
            profile_app = True
            trace_app = True
        case _:
            raise Exception(f'unrecognized --profile "{profile}"')

    trace_proxy = profile == 'trace-proxy' or profile == 'trace-all'
    trace_app = profile == 'trace-app' or profile == 'trace-all'
    profile_app = trace_app or profile == 'app'

    if trace_proxy:
        raise Exception('trace proxy not implemented')

    profile = ''
    app_dir = 'host-rel'
    if profile_app:
        htk_path = os.path.join(path, 'profile')
        trace_opt = '-t' if trace_app else '-ds'
        profile = f'$(which hpcrun) -o {htk_path} {trace_opt} -e instructions -e LLC-loads -e LLC-load-misses -e BLOCKTIME -e CPUTIME'
        app_dir = 'host-htk'

    # Use exec to ensure that .kill() affects the mpirun and not just the shell
    # process, since wer are using Popen(shell=True)
    # https://stackoverflow.com/a/13143013

    # The timeout command could be used on either one of our children
    # setting the parameter to 0 disables it (default)

    proxy_cmd = (f'exec timeout {timeout} '
        f'mpirun -np 2 -H {hosts} '
        f'-bind-to none env '
        f'OMP_NUM_THREADS={config["threads_proxy"]} BUDDY_QUIET_TIME={config["idle_timeout"]} BUDDY_D2D_SIZE={config["bufsize_remote"]} BUDDY_BUFCOUNT_REMOTE={config["bufcount_proxy_remote"]} BUDDY_BUFCOUNT_LOCAL={config["bufcount_proxy_local"]} '
        f'{numactl} {build}/src/buddy-proxy')

    app_cmd = (f'exec mpirun -np 32 -H intel01:16,intel02:16 '
        f'env BUDDY_SENDBUF={config["bufcount_host"]} BUDDY_RECVBUF={config["bufcount_host"]} BUDDY_BUFSIZE={config["bufsize_local"]} '
        f'{app_pre} numactl -N0 {profile} '
        f'{app_dir}/{app_cmds[config["app"]]}')

    ret = 0
    if dry_run:
        print(proxy_cmd)
        print(app_cmd)
    else:
        print(path)

        proxy_log = open(os.path.join(path, 'proxy.log'), 'w')
        app_log = open(os.path.join(path, 'app.log'), 'w')

        children = []
        try:
            print(proxy_cmd)
            proxy_proc = subprocess.Popen(proxy_cmd, shell=True, stdout=proxy_log, stderr=subprocess.STDOUT)

            children = [proxy_proc]

            time.sleep(0.1)

            print(app_cmd)
            app_proc = subprocess.Popen(app_cmd, shell=True, stdout=app_log, stderr=subprocess.STDOUT)

            children.append(app_proc)

            for i in range(2):
                pid, status = os.wait()
                if os.WIFEXITED(status):
                    code = os.WEXITSTATUS(status)
                    if code != 0:
                        ret = code
                        break
                if os.WIFSIGNALED(status):
                    raise ChildKilled()
        finally:
            for child in children:
                child.kill()
                child.wait()

        # for tool in ("hpcstruct", "hpcprof"):
        #     log = open(os.path.join(path, f'{tool}.log'), 'w')
        #     cmd = [tool, htk_path]
        #     print(" ".join(cmd))
        #     subprocess.run(cmd, check=True, stderr=subprocess.STDOUT, stdout=log)

    return ret

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

def log_trial(output, config, res):
    trial = pd.DataFrame([config | res])

    with open(output, 'a') as out:
        if out.tell() == 0:
            htk_cols = ["binary", "instructions_I", "instructions_E", "LLC-loads_I", "LLC-loads_E", "LLC-load-misses_I", "LLC-load-misses_E", "CPUTIME_E", "CPUTIME_I"]
            header = trial.columns.to_list() + htk_cols
            print('\t'.join(header), file=out)

        trial.to_csv(out, index=False, sep='\t', header=False)

def single_run(config, output, dry_run=False, profile='', record_error=False, timeout='10m'):
    # sort config to normalize the order of columns in the output.
    # otherwise we get problems when config is generated by different code
    # paths, eg main() and optimize()
    config = {'uid': uuid.uuid4().hex} | dict(sorted(config.items()))

    if not dry_run and not os.system('pgrep mpirun > /dev/null'):
        raise Exception('mpirun already running!')

    path = os.path.join('app_results', config['uid'])
    if not dry_run:
        os.makedirs(path)

    status = run_trial(config, path, dry_run, profile, timeout=timeout)

    if not dry_run:
        res = {'status': status}
        if status == 0:
            res |= collect_metrics(config, path)

        if status == 0 or record_error:
            log_trial(output, config, res)

        if status != 0:
            raise ChildCrashed()

def df_select(df, kvs):
    mask = pd.Series([True] * len(df))
    for col, val in kvs.items():
        mask &= (df[col] == val)
    return df[mask]

def optimize(fixed, opts, output):
    df = pd.read_csv(output, sep='\t')
    # need to reset index to call df_select() on it again later
    df = df_select(df, fixed).reset_index()

    best = df.loc[df['time'].idxmin()]
    print(best)
    print()

    timeout = 2*best['time']

    threshold = 1.05
    good = df[df['time'] / best['time'] < threshold]

    frontier = []
    seen = set()

    for idx, row in good.iterrows():
        base = {key: row.at[key] for key in opts}
        print(base)
        print()

        for key in opts:
            curr = row[key]
            idx = opts[key].index(curr)
            for off in (-1, +1):
                try:
                    candidate = base.copy()
                    candidate[key] = opts[key][idx+off]
                    s = str(candidate)
                    if not s in seen:
                        seen.add(s)
                        frontier.append(candidate)
                except IndexError:
                    print(f'warning: frontier hit bounds for option {key}')

    # To avoid bias in search direction
    random.shuffle(frontier)

    new = 0
    skipped = 0
    failed = 0
    print(f'frontier size {len(frontier)}')
    for x in frontier:
        if len(df_select(df, x)):
            skipped += 1
        else:
            try:
                single_run(fixed|x, output, record_error=True, timeout=timeout)
                new += 1

                # Early stop to go back an re-evaluate the threshold
                break
            except ChildCrashed:
                failed += 1
    print(f'{new} new runs, {skipped} skipped runs, {failed} failed runs')

    return new

def main():
    p = argparse.ArgumentParser()

    p.add_argument('output')
    p.add_argument('app')
    p.add_argument('offload')

    p.add_argument('-S', '--bufsize-remote', default='')
    p.add_argument('-s', '--bufsize-local', default='')

    p.add_argument('-C', '--bufcount-proxy-remote', default='1')
    p.add_argument('-c', '--bufcount-proxy-local', default='1')
    p.add_argument('-H', '--bufcount-host', default='1')

    p.add_argument('-t', '--threads-proxy', default='8')
    p.add_argument('-i', '--idle-timeout', default='')

    p.add_argument('-P', '--profile')
    p.add_argument('-n', '--dry-run', action='store_true')
    p.add_argument('-r', '--repeat', default=1, type=int)
    p.add_argument('-R', '--randomize', action='store_true')
    p.add_argument('-O', '--optimize', action='store_true')
    p.add_argument('-E', '--record-errors', action='store_true')

    args = p.parse_args()

    if os.system('which mpirun > /dev/null'):
        raise Exception('Cannot find mpirun')

    if os.system('which hpcrun > /dev/null'):
        raise Exception('Cannot find hpctoolkit')

    if args.optimize:
        params = [arg.split(',') for arg in (args.app, args.offload)]
        progress = True

        while progress:
            progress = False
            iters = itertools.product(*params)

            for app, offload in iters:
                fixed = {
                    'app': app,
                    'offload': offload,
                }
                opts = {
                    'bufsize_remote': [2**n for n in range(14, 25)],
                    'bufsize_local': [2**n for n in range(14, 25)],
                    'bufcount_proxy_remote': [2**n for n in range(0, 7)],
                    'bufcount_proxy_local': [2**n for n in range(0, 7)],
                    'bufcount_host': [2**n for n in range(0, 7)],
                    'threads_proxy': list(range(1, 8+1)),
                    'idle_timeout': [2**n for n in range(-10, 1)],
                }
                if optimize(fixed, opts, args.output) > 0:
                    progress = True
        return

    for i in range(args.repeat):
        params = [arg.split(',') for arg in (args.app, args.offload, args.bufsize_remote, args.bufsize_local, args.bufcount_proxy_remote, args.bufcount_proxy_local, args.bufcount_host, args.threads_proxy, args.idle_timeout)]

        if args.randomize:
            iters = [(random.choice(p) for p in params)]
        else:
            iters = itertools.product(*params)

        for app, offload, bufsize_remote, bufsize_local, bufcount_proxy_remote, bufcount_proxy_local, bufcount_host, threads_proxy, idle_timeout in iters:
            config = {
                'app': app,
                'offload': offload,
                'bufsize_remote': bufsize_remote,
                'bufsize_local': bufsize_local,
                'bufcount_proxy_remote': bufcount_proxy_remote,
                'bufcount_proxy_local': bufcount_proxy_local,
                'bufcount_host': bufcount_host,
                'threads_proxy': threads_proxy,
                'idle_timeout': float(idle_timeout) if idle_timeout else '',
            }

            single_run(config, args.output, dry_run=args.dry_run, profile=args.profile, record_error=args.record_errors)

if __name__ == '__main__':
    main()
