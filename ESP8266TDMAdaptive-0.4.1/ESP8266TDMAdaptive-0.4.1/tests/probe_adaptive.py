#!/usr/bin/env python3
"""Record autonomous rate exploration; does not claim a rate is validated."""
import argparse
from hardware_tdm import Bench

parser = argparse.ArgumentParser()
parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
parser.add_argument('--seconds', type=int, default=240)
parser.add_argument('--output', default='adaptive-probe.json')
args = parser.parse_args()
bench = Bench(args)
passed = False
try:
    for board in range(2):
        bench.command(board, '/reset')
    bench.wait(lambda: all(bench.sessions), 15, 'both_booted')
    bench.wait(bench.both_locked, 90, 'initial_lock')
    bench.collect_for(args.seconds)
    passed = not bench.errors and bench.both_locked()
finally:
    bench.save(passed)
