#!/usr/bin/env python3

import argparse
import pandas as pd

pd.options.display.float_format = '{:.2f}'.format

def main():
    p = argparse.ArgumentParser()
    p.add_argument('input')
    args = p.parse_args()

    df = pd.read_csv(args.input, sep='\t') 

    df['goodput'] = 1e-9 * df['bytes_out_local'] / df['time']
    df['l3_hit_rate'] = 1 - df['LLC-load-misses_E'] / df['LLC-loads_E']
    df['intensity'] = 1e-6 * df['instructions_E'] / df['count_out_local']

    config = ['app', 'offload', 'bufs', 'threads']
    group = df.groupby(config)

    print(group.agg({metric: ['mean', 'std', 'count'] for metric in ['goodput', 'l3_hit_rate', 'intensity']}))

if __name__ == '__main__':
    main()
