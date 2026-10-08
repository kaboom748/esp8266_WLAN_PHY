#!/usr/bin/env python3
"""Verify user-entered serial messages cross the RF link in both directions."""
import argparse

from hardware_tdm import Bench


def run(bench):
    for board in range(2):
        bench.command(board, '/reset')
    bench.wait(lambda: all(bench.sessions), 15, 'both_booted')
    for board in range(2):
        bench.command(board, '/auto off')
    bench.wait(bench.both_locked, 90, 'empty_cells_locked')
    bench.wait(lambda: all(s.get('queue') == 0 for s in bench.status), 30, 'queues_drained')
    offset = len(bench.events)
    before = [s['ack'] for s in bench.status]
    texts = ['Bonjour B', 'Bonjour A']
    for board, message in enumerate(texts):
        bench.command(board, '/send ' + message)

    def received_and_acked():
        return all(any(e.get('board') == 1-board
                       and e.get('text', '').startswith('RXDATA ')
                       and ('hex=' + message.encode('ascii').hex() + ' ') in e['text']
                       and 'valid=1' in e['text']
                       for e in bench.events[offset:]) for board, message in enumerate(texts)) and all(
                           s.get('ack', 0) >= old+1 for s, old in zip(bench.status, before))

    bench.wait(received_and_acked, 45, 'bonjour_in_both_directions_exact_and_acked')
    assert not bench.errors, bench.errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    bench = Bench(args)
    passed = False
    try:
        run(bench)
        passed = True
    except Exception as exc:
        bench.errors.append(str(exc))
        raise
    finally:
        for board in range(2):
            bench.command(board, '/auto on')
        bench.collect_for(.3)
        bench.save(passed)
