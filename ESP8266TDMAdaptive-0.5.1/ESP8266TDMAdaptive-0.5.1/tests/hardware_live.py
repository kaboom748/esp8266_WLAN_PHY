#!/usr/bin/env python3
"""Check manual payloads after opening the USB serial ports."""
import argparse
from hardware_tdm import Bench

parser = argparse.ArgumentParser()
parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
parser.add_argument('--output', required=True)
args = parser.parse_args()
bench = Bench(args)
passed = False
try:
    # A CH340 bridge may pulse reset on open even with DTR/RTS requested low.
    bench.wait(lambda: all(bench.sessions) or all(bench.status), 15, 'serial_ready')
    boots = bench.boots[:]
    for board in range(2):
        bench.command(board, '/auto off')
        bench.command(board, '/stats')
    bench.wait(lambda: bench.both_locked() and all(s.get('queue') == 0 for s in bench.status),
               90, 'running_link_drained')
    offset = len(bench.events)
    before = [s['ack'] for s in bench.status]
    messages = ['IQ35-A-012345', 'IQ35-B-543210']
    for board, message in enumerate(messages):
        bench.command(board, '/send ' + message)
    def delivered():
        return all(any(e.get('board') == 1-board
                       and ('hex=' + message.encode('ascii').hex() + ' ') in e.get('text', '')
                       for e in bench.events[offset:]) for board, message in enumerate(messages)) and all(
                           s.get('ack', 0) > old for s, old in zip(bench.status, before))
    bench.wait(delivered, 20, 'manual_payloads_exact_and_acked')
    assert not bench.errors and bench.boots == boots
    passed = True
except Exception as exc:
    bench.errors.append(str(exc))
    raise
finally:
    for board in range(2):
        bench.command(board, '/auto on')
    bench.collect_for(2.5)
    bench.save(passed)
