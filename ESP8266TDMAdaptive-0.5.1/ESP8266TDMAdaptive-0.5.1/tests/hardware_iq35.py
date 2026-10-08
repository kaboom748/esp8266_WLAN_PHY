#!/usr/bin/env python3
"""Measure useful deliveries, filled slots and RX cadence on both real boards."""
import argparse
from hardware_tdm import Bench, fields

parser = argparse.ArgumentParser()
parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
parser.add_argument('--seconds', type=int, default=60)
parser.add_argument('--limit', type=int, default=6)
parser.add_argument('--allow-retries', action='store_true', help='Validate reliable delivery; preserve raw loss counters')
parser.add_argument('--output', required=True)
args = parser.parse_args()
bench = Bench(args)
passed = False
try:
    for board in range(2):
        bench.command(board, '/reset')
    bench.wait(lambda: all(bench.sessions), 15, 'both_booted')
    for board in range(2):
        bench.command(board, '/limit ' + str(args.limit))
        bench.command(board, '/auto on')
        bench.command(board, '/stats')
    bench.wait(bench.both_locked, 30, 'initial_lock')
    bench.wait(lambda: bench.both_locked() and all(s.get('rate') == args.limit for s in bench.status),
               100, 'target_rate_reached')
    before = [s.copy() for s in bench.status]
    offset = len(bench.events)
    bench.collect_for(args.seconds)
    assert bench.both_locked(), 'link lost'
    assert all(s.get('rate') == args.limit for s in bench.status), 'target rate not sustained'
    assert all(s.get('txFail') == 0 for s in bench.status), 'TX aborted'
    assert all(n >= 10 for n in bench.payload_count), 'too few verified deliveries'
    losses = [s['miss']-old['miss'] for s, old in zip(bench.status, before)]
    print('RAW_LOSSES_DURING_SOAK ' + str(losses), flush=True)
    if not args.allow_retries:
        assert not any(losses), 'losses during target-rate soak'
    assert all(s['rx']-old['rx'] >= args.seconds//2-3
               for s, old in zip(bench.status, before)), 'insufficient deliveries during soak'
    recorded_rates = [fields(e['text']).get('rate') for e in bench.events[offset:]
                      if e.get('text', '').startswith('TDM ')]
    assert all(rate == args.limit for rate in recorded_rates if rate is not None), 'rate fell during soak'
    for board in range(2):
        iq = [fields(e['text']) for e in bench.events if e.get('board') == board
              and e.get('text', '').startswith('IQ35 ')]
        windows = [fields(e['text']) for e in bench.events if e.get('board') == board
                   and e.get('text', '').startswith('WINDOW ')]
        assert iq and iq[-1]['estimatesPerSec'] >= 35700, 'RX cadence below 35.7k/s'
        assert windows and windows[-1]['fillBits'] > 1000, 'no filler'
        if board == 1:
            assert windows[-1]['syncs'] > 10, 'clock not synchronized repeatedly'
            assert abs(int(windows[-1]['phaseUs'])) < 10000, 'clock phase error'
    assert bench.boots == [1, 1], 'unexpected reboot'
    for board in range(2):
        bench.command(board, '/auto off')
    bench.wait(lambda: all(s.get('queue') == 0 for s in bench.status), 20, 'all_messages_acked')
    bench.collect_for(2.5)
    assert all(s['ack'] == bench.status[1-board]['delivered'] for board, s in enumerate(bench.status)), 'delivery/ACK totals differ'
    assert not bench.errors
    passed = True
except Exception as exc:
    bench.errors.append(str(exc))
    raise
finally:
    for board in range(2):
        bench.command(board, '/auto on')
    bench.collect_for(.5)
    bench.save(passed)
