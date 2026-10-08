#!/usr/bin/env python3
"""Validate real RF exchanges, payloads, empty cells and automatic recovery."""
import argparse
import datetime
import json
import re
import time
from pathlib import Path

import serial


def fields(line):
    values = dict(re.findall(r'(\w+)=([^ ]+)', line))
    for key, value in list(values.items()):
        if value.isdecimal():
            values[key] = int(value)
    return values


class Bench:
    def __init__(self, args):
        self.args = args
        self.ports = []
        for path in args.ports:
            port = serial.Serial(baudrate=115200, timeout=0)
            port.dtr = False
            port.rts = False
            port.port = path
            port.open()
            self.ports.append(port)
        self.buffers = [b'', b'']
        self.status = [{}, {}]
        self.phy = [{}, {}]
        self.adapt = [{}, {}]
        self.last_status_at = [0, 0]
        self.sessions = [None, None]
        self.events, self.phases, self.errors, self.warnings = [], [], [], []
        self.messages = set()
        self.payload_count = [0, 0]
        self.boots = [0, 0]
        self.started = time.monotonic()
        self.progress_at = 0

    def command(self, board, command):
        self.ports[board].write((command + '\n').encode('ascii'))
        self.events.append(dict(t=self.elapsed(), board=board, command=command))

    def elapsed(self):
        return round(time.monotonic() - self.started, 3)

    def collect(self):
        for board, port in enumerate(self.ports):
            self.buffers[board] += port.read(port.in_waiting or 1)
            while b'\n' in self.buffers[board]:
                raw, self.buffers[board] = self.buffers[board].split(b'\n', 1)
                line = raw.decode('ascii', errors='replace').strip()
                if not line:
                    continue
                self.events.append(dict(t=self.elapsed(), board=board, text=line))
                if 'Exception (' in line or 'wdt reset' in line.lower():
                    self.errors.append('ESP reset: ' + line)
                if line.startswith('TDM_READY '):
                    self.sessions[board] = fields(line)['session']
                    self.boots[board] += 1
                    self.status[board] = {}
                elif line.startswith('TDM '):
                    data = fields(line)
                    required = ('role', 'cycle', 'tx', 'rx', 'miss', 'txFail', 'ack', 'delivered',
                                'retry', 'dup', 'queue', 'resync', 'corr', 'peerCorr', 'heap', 't',
                                'rate', 'bitUs', 'limit', 'up', 'down', 'recovery')
                    if data.get('state') not in ('SEARCH', 'SYNC', 'LOCKED') or not all(
                            isinstance(data.get(k), int) for k in required):
                        self.warnings.append(dict(t=self.elapsed(), board=board, truncated_status=line))
                        continue
                    self.status[board] = data
                    self.last_status_at[board] = self.elapsed()
                elif line.startswith('ADAPT '):
                    data = fields(line)
                    required = ('role', 'rate', 'next', 'waitMs', 'good', 'qualityQ4', 'bad', 'losses')
                    if all(isinstance(data.get(k), int) for k in required):
                        self.adapt[board] = data
                    else:
                        self.warnings.append(dict(t=self.elapsed(), board=board, truncated_adapt=line))
                elif line.startswith('PHY '):
                    self.phy[board] = fields(line)
                elif line.startswith('RXDATA '):
                    data = fields(line)
                    required = ('role', 'id', 'len', 'hex', 'valid')
                    if not all(key in data for key in required):
                        raise AssertionError('truncated RXDATA diagnostic: ' + line)
                    payload = bytes.fromhex(str(data['hex']))
                    if len(payload) != data['len'] or data['valid'] != 1:
                        self.errors.append('invalid length or firmware validation: ' + line)
                    if len(payload) == 14 and payload[0] == 0xa5:
                        peer_role = 2 - board
                        seq = int.from_bytes(payload[2:6], 'little')
                        expected = bytes([0xa5, peer_role]) + seq.to_bytes(4, 'little')
                        expected += bytes(((seq * 17 + i * 29) & 255) ^ peer_role for i in range(6, 14))
                        if payload != expected:
                            self.errors.append('independent payload mismatch: ' + line)
                    key = (board, self.sessions[board], self.sessions[1-board], data['id'])
                    if key in self.messages:
                        self.errors.append('duplicate application delivery: ' + line)
                    self.messages.add(key)
                    self.payload_count[board] += 1
        if self.elapsed() - self.progress_at > 15:
            self.progress_at = self.elapsed()
            snapshot = [{k: s.get(k) for k in ('state', 'cycle', 'rx', 'miss', 'ack', 'retry', 'bitUs', 'up', 'down', 'corr', 'peerCorr')} for s in self.status]
            print('PROGRESS ' + json.dumps(dict(t=self.elapsed(), boards=snapshot, adapt=self.adapt)), flush=True)
        time.sleep(.002)

    def wait(self, predicate, timeout, label):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.collect()
            if self.errors:
                raise AssertionError(self.errors[-1])
            if predicate():
                self.phases.append(dict(name=label, passed=True, t=self.elapsed(), status=[s.copy() for s in self.status]))
                print('PASS ' + label, flush=True)
                return
        raise AssertionError('timeout: ' + label)

    def collect_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.collect()

    def both_locked(self):
        return all(s.get('state') == 'LOCKED' for s in self.status)

    def run(self):
        for i in range(2):
            self.command(i, '/reset')
        self.wait(lambda: all(self.sessions), 15, 'both_booted')
        self.wait(self.both_locked, 90, 'initial_lock')
        self.wait(lambda: all(s.get('ack', 0) >= self.args.cells for s in self.status),
                  self.args.cells * 10 + 60, 'bidirectional_data_soak')
        assert all(s.get('txFail', 0) == 0 for s in self.status)
        for i in range(2):
            self.command(i, '/auto off')
        self.wait(lambda: all(s.get('queue') == 0 for s in self.status), 60, 'queues_drained')
        baseline = [s.copy() for s in self.status]
        self.collect_for(22)
        assert all(s['rx'] - b['rx'] >= 3 and s['delivered'] == b['delivered']
                   for s, b in zip(self.status, baseline))
        self.phases.append(dict(name='empty_cells_continue', passed=True, t=self.elapsed()))
        print('PASS empty_cells_continue', flush=True)
        texts = ['TDM-A-01234567', 'TDM-B-76543210']
        offset = len(self.events)
        ack_before = [s['ack'] for s in self.status]
        for i, text in enumerate(texts):
            self.command(i, '/send ' + text)
        def manual_delivered():
            return all(any(e.get('board') == 1-i and 'hex=' + text.encode().hex() in e.get('text', '')
                           for e in self.events[offset:]) for i, text in enumerate(texts)) and all(
                               s.get('ack', 0) > old for s, old in zip(self.status, ack_before))
        self.wait(manual_delivered, 60, 'manual_payloads_exact_and_acked')
        self.command(1, '/pause')
        self.wait(lambda: self.status[0].get('state') == 'SEARCH', 40, 'loss_detected')
        # Queue a real message while no ACK can return.
        self.command(0, '/send retained')
        self.collect_for(12)
        assert self.status[0].get('queue') == 1
        self.command(1, '/run')
        self.wait(lambda: self.both_locked() and self.status[0].get('queue') == 0, 90, 'outage_recovery_queue_retained')
        for board in (1, 0):
            previous = self.sessions[board]
            self.command(board, '/reset')
            self.wait(lambda: self.sessions[board] != previous, 15, 'reboot_' + str(board+1))
            self.wait(self.both_locked, 90, 'relock_after_reboot_' + str(board+1))
        baseline_rx = [s['rx'] for s in self.status]
        for i in range(2):
            self.command(i, '/recal')
        self.wait(lambda: self.both_locked() and all(s.get('rx', 0) >= old+4 for s, old in zip(self.status, baseline_rx)),
                  90, 'cold_calibration_reacquired')
        for i in range(2):
            self.command(i, '/auto on')
        self.collect_for(12)
        assert not self.errors
        assert self.boots == [2, 2], self.boots
        assert all(s.get('txFail', 0) == 0 for s in self.status)
        assert all(s.get('abort', 0) == 0 for s in self.phy)

    def save(self, passed):
        result = dict(passed=passed, utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      parameters=vars(self.args), elapsed=self.elapsed(), phases=self.phases,
                      status=self.status, phy=self.phy, adapt=self.adapt, errors=self.errors, warnings=self.warnings,
                      independently_checked_deliveries=self.payload_count, boots=self.boots, events=self.events)
        Path(self.args.output).write_text(json.dumps(result, indent=2) + '\n')
        for port in self.ports:
            port.close()
        print('RESULT ' + json.dumps({k: result[k] for k in ('passed', 'elapsed', 'independently_checked_deliveries', 'boots', 'errors')}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'], help='master then follower')
    parser.add_argument('--cells', type=int, default=100)
    parser.add_argument('--output', default='tdm-hardware-results.json')
    args = parser.parse_args()
    bench = Bench(args)
    passed = False
    try:
        bench.run()
        passed = True
    except Exception as exc:
        bench.errors.append(str(exc))
        raise
    finally:
        bench.save(passed)
