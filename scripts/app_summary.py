#!/usr/bin/env python3

import argparse
import pandas as pd

pd.options.display.float_format = '{:.2f}'.format

def main():
    p = argparse.ArgumentParser()
    p.add_argument('input')
    p.add_argument('-c', '--clean', action='store_true')
    args = p.parse_args()

    df = pd.read_csv(args.input, sep='\t') 

    df['goodput'] = 1e-9 * df['bytes_out_local'] / df['time']
    df['l3_hit_rate'] = 1 - df['LLC-load-misses_E'] / df['LLC-loads_E']
    df['intensity'] = 1e-6 * df['instructions_E'] / df['count_out_local']

    num_out = df['count_out_local'] + df['count_out_remote']
    df['quiet_flush_ratio'] = df['quiet_flush_bufs'] / num_out

    df['h2d_eff'] = df['avg_in_local'] / df['h2d_size']
    df['d2h_eff'] = df['avg_out_local'] / df['d2h_size']
    df['d2d_eff'] = df['avg_out_remote'] / df['d2d_size']

    config = ['app', 'offload', 'bufs', 'threads']
    group = df.groupby(config)

    aggs = ['mean', 'std']
    if not args.clean:
        aggs.append('count')

    res = group.agg({metric: aggs for metric in ['goodput', 'l3_hit_rate', 'intensity', 'quiet_flush_ratio', 'h2d_eff', 'd2h_eff', 'd2d_eff']})
    print(res)

if __name__ == '__main__':
    main()
