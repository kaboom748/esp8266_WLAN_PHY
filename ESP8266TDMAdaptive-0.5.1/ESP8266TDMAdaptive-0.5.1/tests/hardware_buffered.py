#!/usr/bin/env python3
"""Compare useful buffered throughput in fixed slots on both real ESP8266s."""
import argparse
from hardware_tdm import Bench, fields

parser = argparse.ArgumentParser()
parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
parser.add_argument('--seconds', type=int, default=60)
parser.add_argument('--limit', type=int, default=6)
parser.add_argument('--output', required=True)
args = parser.parse_args()
bench = Bench(args)
passed = False


def latest(prefix, board):
    for event in reversed(bench.events):
        if event.get('board') == board and event.get('text', '').startswith(prefix + ' '):
            return fields(event['text'])
    return {}


def verify_records():
    for board in range(2):
        records = [fields(e['text']) for e in bench.events
                   if e.get('board') == board and e.get('text', '').startswith('RXDATA ')]
        demo = [bytes.fromhex(str(r['hex'])) for r in records]
        sequences = [int.from_bytes(p[2:6], 'little') for p in demo if len(p) == 14 and p[0] == 0xa5]
        assert sequences == list(range(len(sequences))), ('lost, reordered or duplicated stream', board, sequences)
        assert len(records) == bench.status[board]['delivered'], ('missing delivery logs', board)


try:
    # CH340 may reset on open. Do not issue commands before firmware is ready.
    bench.wait(lambda: all(bench.sessions[i] or bench.status[i] for i in range(2)), 20, 'serial_ready')
    boots = bench.boots.copy()
    for board in range(2):
        bench.command(board, '/reset')
    bench.wait(lambda: all(n > old for n, old in zip(bench.boots, boots)), 20, 'both_rebooted')
    boots = bench.boots.copy()
    # Exclude pre-reset application records from the exact stream comparison.
    bench.events = [e for e in bench.events if e.get('text', '').startswith('TDM_READY ')]
    for board in range(2):
        bench.command(board, '/limit 0')
        bench.command(board, '/auto on')
        bench.command(board, '/stats')
    bench.wait(lambda: bench.both_locked() and all(s.get('rate') == 0 for s in bench.status),
               40, 'base_rate_locked')
    before = [s.copy() for s in bench.status]
    bench.collect_for(12)
    assert all(s['delivered'] > old['delivered'] + 2 for s, old in zip(bench.status, before))
    for board in range(2):
        window = latest('WINDOW', board)
        assert window['frames'] == 1 and window['bytes'] == 14, window
        assert window['fillBits'] == 0 and window['queuedBytes'] > 0, window
    bench.phases.append(dict(name='slow_slots_one_packet_backlog_retained_no_filler', passed=True,
                             t=bench.elapsed(), windows=[latest('WINDOW', i) for i in range(2)]))
    print('PASS slow_slots_one_packet_backlog_retained_no_filler', flush=True)
    for board in range(2):
        bench.command(board, '/limit ' + str(args.limit))
    bench.wait(lambda: bench.both_locked() and all(s.get('rate') == args.limit for s in bench.status),
               140, 'target_rate_reached')
    before = [s.copy() for s in bench.status]
    offset = len(bench.events)
    start = bench.elapsed()
    bench.collect_for(args.seconds)
    assert bench.both_locked(), 'link lost'
    samples = [fields(e['text']) for e in bench.events[offset:] if e.get('text', '').startswith('TDM ')]
    assert samples and all(s.get('rate') == args.limit for s in samples), 'rate fell during soak'
    useful = [s['delivered']-old['delivered'] for s, old in zip(bench.status, before)]
    assert all(n >= args.seconds * 2 for n in useful), ('useful throughput too low', useful)
    for board in range(2):
        window = latest('WINDOW', board)
        assert window['frames'] >= 5 and window['bytes'] >= 70, window
        assert window['fillBits'] == 0 and window['logDrop'] == 0, window
        assert abs(int(window['phaseUs'])) < 10000, window
        assert latest('IQ35', board)['estimatesPerSec'] >= 35700, latest('IQ35', board)
    bench.phases.append(dict(name='useful_throughput_soak', passed=True, seconds=bench.elapsed()-start,
                             useful_packets=useful, useful_bytes=[n*14 for n in useful],
                             before=before, after=[s.copy() for s in bench.status],
                             windows=[latest('WINDOW', i) for i in range(2)],
                             iq=[latest('IQ35', i) for i in range(2)]))
    print('PASS useful_throughput_soak ' + str(useful), flush=True)
    for board in range(2):
        bench.command(board, '/auto off')
    bench.wait(lambda: all(s.get('queue') == 0 for s in bench.status), 40, 'all_buffers_drained_and_acked')
    bench.collect_for(6)
    assert all(s['ack'] == bench.status[1-board]['delivered'] for board, s in enumerate(bench.status))
    assert all(s.get('txFail') == 0 for s in bench.status), 'TX failures'
    assert all(s.get('abort') == 0 for s in bench.phy), 'TX aborts'
    assert bench.boots == boots, 'unexpected reboot'
    assert not bench.warnings, bench.warnings
    verify_records()
    bench.phases.append(dict(name='exact_ordered_stream_both_directions', passed=True, t=bench.elapsed()))
    offset = len(bench.events)
    ack_before = [s['ack'] for s in bench.status]
    messages = ['BUFFER-A-1234', 'BUFFER-B-4321']
    for board, message in enumerate(messages):
        bench.command(board, '/send ' + message)
    def manual_delivered():
        return all(any(e.get('board') == 1-board
                       and ('hex=' + message.encode('ascii').hex() + ' ') in e.get('text', '')
                       for e in bench.events[offset:]) for board, message in enumerate(messages)) and all(
                           s.get('ack', 0) > old for s, old in zip(bench.status, ack_before))
    bench.wait(manual_delivered, 25, 'manual_payloads_exact_and_acked')
    assert all(latest('WINDOW', i).get('logDrop') == 0 for i in range(2))
    assert not bench.warnings, bench.warnings
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
