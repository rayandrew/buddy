#!/usr/bin/env python3

import argparse
import pandas as pd
import subprocess
import time
import os

p = argparse.ArgumentParser()

p.add_argument('output')

# p.add_argument('server_node')
# p.add_argument('server_dev')
# p.add_argument('server_numa')
# p.add_argument('client_node')
# p.add_argument('client_dev')
# p.add_argument('client_numa')

# args = p.parse_args()

def run(args, cmd):
    def base_cmd(node, device, numa):
        return ['ssh', node, 'numactl', '-N', numa] + cmd + ['-d', device, '-x', '0']

    server_cmd = base_cmd(args.server_node, args.server_dev, args.server_numa)
    client_cmd = base_cmd(args.client_node, args.client_dev, args.client_numa) + [args.server_node]

    server_proc = subprocess.Popen(server_cmd, text=True, stdout=subprocess.PIPE)
    time.sleep(0.5)

    client_proc = subprocess.Popen(client_cmd, text=True, stdout=subprocess.PIPE)

    for i in range(2):
        pid, status = os.wait()
        if os.WIFEXITED(status):
            code = os.WEXITSTATUS(status)
            if code != 0:
                raise Exception(f'Non-zero exit code {code}')
        if os.WIFSIGNALED(status):
            raise Exception(f'Signal {os.WTERMSIG(status)}')

    #print(server_proc.stdout.read())
    return client_proc.stdout.read()

def parse_res(s, index):
    lines = s.splitlines()
    i = 0
    while not lines[i].startswith(' #bytes'):
        i += 1
    data = lines[i+1].split()
    return pd.Series(data, index)

lat_index = ['lat_bytes', 'lat_iters', 'lat_min', 'lat_max', 'lat_typical', 'lat_avg', 'lat_stev', 'lat_p99', 'lat_p99.9']
bw_index = ['bw_bytes', 'bw_iters', 'bw_peak', 'bw_avg', 'bw_rate']

lat_args = ['-s', '1']
bw_args = []

def main(args):
    lat_res = run(args, ['ib_send_lat'] + lat_args)
    bw_res = run(args, ['ib_send_bw'] + bw_args)

    lat_data = parse_res(lat_res, lat_index)
    bw_data = parse_res(bw_res, bw_index)

    config = vars(args).copy()
    del config['output']
    config = pd.Series(config)

    row = pd.DataFrame([pd.concat([config, lat_data, bw_data])])
    with open(args.output, 'a') as out:
        fresh = out.tell() == 0
        row.to_csv(out, index=False, header=fresh)

for server in ('host', 'bf'):
    for client in ('host', 'bf'):
        for mode in ('intra', 'inter'):
            server_node = 'sm2'
            if server == 'bf':
                server_node += '-bf'

            client_node = 'sm3'
            if mode == 'intra':
                client_node = 'sm2'
            if client == 'bf':
                client_node += '-bf'

            numa = {'host': ['0', '1'], 'bf': ['0']}
            dev = {'host': ['mlx5_0', 'mlx5_2'], 'bf': ['mlx5_2']}

            server_devs = dev[server]
            server_numas = numa[server]

            client_devs = dev[client]
            client_numas = numa[client]

            for sd in server_devs:
                for sn in server_numas:
                    for cd in client_devs:
                        for cn in client_numas:
                            a = p.parse_args()

                            a.server_node = server_node
                            a.server_dev = sd
                            a.server_numa = sn

                            a.client_node = client_node
                            a.client_dev = cd
                            a.client_numa = cn

                            main(a)
