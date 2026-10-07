#!/usr/bin/env python3
"""Bounded diagnostic: verify both gain readbacks before comparing RF reception."""
import argparse
import json
from itertools import product

from hardware_tdm import Bench, fields


def latest(bench, board, prefix, since=0):
    for event in reversed(bench.events):
        if event['t'] < since:
            break
        if event.get('board') == board and event.get('text', '').startswith(prefix):
            return fields(event['text'])
    return None


def paused_snapshot(bench):
    start = bench.elapsed()
    for board in range(2):
        bench.command(board, '/pause')
        bench.command(board, '/stats')
    bench.wait(lambda: all(latest(bench, b, 'RF ', start) and bench.last_status_at[b] >= start
                           for b in range(2)),
               12, 'fresh_paused_readbacks')
    return dict(status=[s.copy() for s in bench.status],
                phy=[s.copy() for s in bench.phy],
                rf=[latest(bench, b, 'RF ', start) for b in range(2)],
                frame=[latest(bench, b, 'FRAME ', start) for b in range(2)],
                iq=[latest(bench, b, 'IQ ', start) for b in range(2)])


def run(bench, args):
    rf_words = [0, 0x40, 0x60, 0x70, 0x78, 0x7c, 0x7f]
    for mode, setting in product(args.modes, args.settings):
        bench.sessions = [None, None]
        for board in range(2):
            bench.command(board, '/reset')
        bench.wait(lambda: all(bench.sessions), 15, 'both_booted')
        paused_snapshot(bench)
        start = bench.elapsed()
        for board in range(2):
            bench.command(board, '/limit 0')
            bench.command(board, '/gain ' + setting)
            bench.command(board, '/iq ' + str(mode))
            bench.command(board, '/stats')

        def applied():
            for board in range(2):
                rf = latest(bench, board, 'RF ', start)
                status = bench.status[board]
                iq = latest(bench, board, 'IQ ', start)
                if not rf or not iq or iq.get('mode') != mode or status.get('limit') != 0:
                    return False
                if setting == 'agc':
                    if rf.get('manual') != 0 or rf.get('rxStop') != 0:
                        return False
                else:
                    gain, vga = map(int, setting.split())
                    # Hex register strings may be parsed as decimal by generic fields().
                    actual = int(str(rf['rfWord']), 16)
                    if (rf.get('manual') != 1 or actual != rf_words[gain]
                            or rf.get('vga') != vga or rf.get('rxStop') != 1):
                        return False
                if any(rf.get(k) != 0 for k in ('apwr', 'ask')):
                    return False
            return True

        bench.wait(applied, 10, 'gain_verified_' + setting.replace(' ', '_'))
        baseline = dict(status=[s.copy() for s in bench.status],
                        phy=[s.copy() for s in bench.phy],
                        rf=[latest(bench, b, 'RF ', start) for b in range(2)])
        if not args.one_way:
            bench.command(1, '/run')
        bench.command(0, '/run')
        measured_at = bench.elapsed()
        bench.collect_for(args.seconds)
        result = paused_snapshot(bench)
        delta = [{k: after.get(k, 0) - before.get(k, 0)
                  for k in ('rx', 'tx', 'ack', 'miss', 'delivered', 'txFail')}
                 for before, after in zip(baseline['status'], result['status'])]
        phase = dict(name='gain_comparison', diagnostic=True, setting=setting, iq_mode=mode,
                     one_way=args.one_way, started=measured_at, ended=bench.elapsed(),
                     baseline=baseline, result=result, delta=delta)
        bench.phases.append(phase)
        print('GAIN_RESULT ' + json.dumps(phase), flush=True)
        if args.require_corrected:
            assert all(d['rx'] >= 2 for d in delta), 'fewer than two received cells per board'
            assert all(f and f['drops'] == 0 for f in result['frame']), 'CRC-valid candidate discarded'
            assert any(e['t'] >= measured_at and e.get('text', '').startswith('TDM ')
                       and fields(e['text']).get('corr', 0) > 0 for e in bench.events), 'no corrected cell observed'
            assert not bench.errors, bench.errors
            phase.update(name='corrected_candidates_delivered', diagnostic=False, passed=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
    parser.add_argument('--settings', nargs='+', default=['agc', '2 2', '4 2', '6 2', 'agc'])
    parser.add_argument('--seconds', type=float, default=36)
    parser.add_argument('--modes', nargs='+', type=int, choices=[0, 1], default=[0])
    parser.add_argument('--one-way', action='store_true')
    parser.add_argument('--require-corrected', action='store_true',
                        help='Require corrected bidirectional delivery without candidate drops')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    bench = Bench(args)
    passed = False
    try:
        run(bench, args)
        passed = args.require_corrected
    except Exception as exc:
        bench.errors.append(str(exc))
        raise
    finally:
        for board in range(2):
            bench.command(board, '/gain agc')
            bench.command(board, '/iq 1')
            bench.command(board, '/limit 5')
            bench.command(board, '/run')
        bench.collect_for(.3)
        # A diagnostic is not a release-validation pass.
        bench.save(passed)
