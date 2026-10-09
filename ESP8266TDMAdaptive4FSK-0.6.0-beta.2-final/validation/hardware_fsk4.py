#!/usr/bin/env python3
"""Serial collector for the 0.6.0-beta.2 4-FSK firmware."""
import argparse
import datetime
import json
import re
import time
from pathlib import Path

import serial


def fields(line):
    result = dict(re.findall(r'(?:^|\s)(\w+)=([^\s]+)', line))
    for key, value in list(result.items()):
        if key not in ('hex', 'session', 'chip', 'version') and re.fullmatch(r'-?\d+', value):
            result[key] = int(value)
    return result


class Bench:
    def __init__(self, args):
        self.args = args
        self.start = time.monotonic()
        self.ports = []
        self.buffers = [b'', b'']
        self.latest = [{}, {}]
        self.boots = [0, 0]
        self.sessions = [None, None]
        self.records = [[], []]
        self.events, self.phases, self.errors, self.warnings = [], [], [], []
        self.next_progress = 15
        try:
            for path in args.ports:
                port = serial.Serial(baudrate=115200, timeout=0, write_timeout=2, exclusive=True)
                port.dtr = False
                port.rts = False
                port.port = path
                port.open()
                self.ports.append(port)
        except Exception:
            for port in self.ports:
                port.close()
            raise

    def elapsed(self):
        return round(time.monotonic() - self.start, 3)

    def status(self, board, prefix='TDM'):
        return self.latest[board].get(prefix, {}).get('data', {})

    def command(self, board, text):
        self.events.append(dict(t=self.elapsed(), board=board, command=text))
        self.ports[board].write((text + '\n').encode('ascii'))

    def command_both(self, text):
        for board in range(2):
            self.command(board, text)

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
                    self.errors.append('reset/exception: ' + line)
                prefix = line.split(' ', 1)[0]
                data = fields(line)
                if prefix == 'TDM_READY':
                    if data.get('version') != '0.6.0-beta.2-4fsk' or data.get('role') != board + 1 or data.get('ok') != 1:
                        self.errors.append('wrong firmware, role or initialization: ' + line)
                    self.boots[board] += 1
                    self.sessions[board] = data.get('session')
                    self.latest[board].clear()
                    self.records[board].clear()
                required = {
                    'TDM': ('role', 'cycle', 'tx', 'rx', 'miss', 'txFail', 'ack', 'delivered', 'retry', 'dup', 'queue', 'resync', 'corr', 'peerCorr', 't', 'rate', 'symbolUs', 'limit', 'recovery'),
                    'PHY': ('role', 'sync', 'crc', 'reject', 'slow', 'abort', 'late', 'workUs', 'samples'),
                    'WINDOW': ('role', 'slotUs', 'guardUs', 'fillBits', 'syncs', 'phaseUs', 'frames', 'bytes', 'queuedBytes', 'deliveredBytes', 'ackBytes', 'logDrop'),
                    'IQ35': ('role', 'estimates', 'rawSamples', 'activeUs', 'estimatesPerSec'),
                    'FSK4': ('role', 'trained', 'symbolUs', 'rawBps', 'calUs'),
                    'IQ': ('role', 'hz', 'e', 'dtNs', 'jitterNs'),
                }
                if prefix in required:
                    valid = all(isinstance(data.get(k), int) for k in required[prefix])
                    valid &= data.get('role') == board + 1
                    if prefix == 'TDM':
                        valid &= data.get('state') in ('SEARCH', 'SYNC', 'LOCKED')
                    if prefix == 'FSK4':
                        valid &= bool(re.fullmatch(r'-?\d+,-?\d+,-?\d+,-?\d+', str(data.get('centers', ''))))
                    if valid:
                        self.latest[board][prefix] = dict(t=self.elapsed(), data=data)
                    else:
                        self.warnings.append(dict(t=self.elapsed(), board=board, malformed=line))
                elif prefix == 'RXDATA':
                    try:
                        payload = bytes.fromhex(data['hex'])
                        assert len(payload) == data['len'] and data['valid'] == 1 and data['role'] == board + 1
                    except (ValueError, KeyError, TypeError, AssertionError):
                        self.warnings.append(dict(t=self.elapsed(), board=board, malformed=line))
                        continue
                    record = dict(t=self.elapsed(), id=data['id'], hex=data['hex'])
                    if len(payload) == 14 and payload[0] == 0xa5:
                        peer = 2 - board
                        seq = int.from_bytes(payload[2:6], 'little')
                        expected = bytes([0xa5, peer]) + seq.to_bytes(4, 'little')
                        expected += bytes(((seq * 17 + i * 29) & 255) ^ peer for i in range(6, 14))
                        if payload != expected:
                            self.errors.append('independent payload mismatch: ' + line)
                        record['sequence'] = seq
                    self.records[board].append(record)
        if self.elapsed() >= self.next_progress:
            self.next_progress += 15
            summary = [dict(status=self.status(i), four=self.status(i, 'FSK4'), phy=self.status(i, 'PHY')) for i in range(2)]
            print('PROGRESS ' + json.dumps(dict(t=self.elapsed(), boards=summary)), flush=True)
        time.sleep(.002)

    def wait(self, predicate, seconds, label):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.collect()
            if self.errors:
                raise AssertionError(self.errors[-1])
            if predicate():
                self.phases.append(dict(name=label, passed=True, t=self.elapsed(), latest=self.snapshot()))
                print('PASS ' + label, flush=True)
                return
        raise AssertionError('timeout: ' + label)

    def collect_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.collect()
            if self.errors:
                raise AssertionError(self.errors[-1])

    def snapshot(self):
        return json.loads(json.dumps(self.latest))

    def locked(self, rate=None):
        return all(self.status(i).get('state') == 'LOCKED'
                   and (rate is None or self.status(i).get('rate') == rate)
                   and self.elapsed() - self.latest[i]['TDM']['t'] < 10 for i in range(2))

    def run(self):
        self.collect_for(1)
        old = self.boots.copy()
        self.command_both('/reset')
        self.wait(lambda: all(a > b for a, b in zip(self.boots, old)) and all(self.sessions), 20, 'both_expected_firmwares_reset_for_clean_run')
        expected_boots = self.boots.copy()
        self.command_both('/limit 0')
        self.command_both('/auto on')
        self.command_both('/stats')
        self.wait(lambda: self.locked(0), 65, 'base_4fsk_locked_both_directions')
        before = [self.status(i).copy() for i in range(2)]
        self.collect_for(12)
        assert all(self.status(i)['delivered'] > before[i]['delivered'] + 2 for i in range(2)), 'no sustained base-rate delivery'
        assert all(self.status(i, 'WINDOW').get('fillBits') == 0 for i in range(2)), 'unexpected filler'
        self.phases.append(dict(name='base_rate_payloads', passed=True, t=self.elapsed(), latest=self.snapshot()))
        self.command_both('/limit ' + str(self.args.limit))
        self.wait(lambda: self.locked(self.args.limit), 170, 'target_4fsk_rate_reached')
        before = [self.status(i).copy() for i in range(2)]
        offset = len(self.events)
        self.collect_for(self.args.seconds)
        assert self.locked(self.args.limit), 'target-rate link lost'
        rates = [fields(e['text']).get('rate') for e in self.events[offset:] if e.get('text', '').startswith('TDM ')]
        assert rates and all(rate == self.args.limit for rate in rates), 'rate fallback during plateau'
        throughput = []
        for i in range(2):
            after = self.status(i)
            delta_ms = (after['t'] - before[i]['t']) & 0xffffffff
            packets = after['delivered'] - before[i]['delivered']
            assert delta_ms > 0 and packets > 0, 'no useful throughput'
            throughput.append(dict(receiving_role=i+1, duration_ms=delta_ms, packets=packets, useful_bytes=packets*14,
                                   bytes_per_second=packets*14000/delta_ms))
        self.phases.append(dict(name='target_rate_plateau', passed=True, t=self.elapsed(), throughput=throughput, latest=self.snapshot()))
        self.command_both('/auto off')
        self.wait(lambda: all(self.status(i).get('queue') == 0 for i in range(2)), 55, 'queues_drained')
        self.collect_for(6)
        for i in range(2):
            s = self.status(i)
            assert s['ack'] == self.status(1-i)['delivered'], 'ACK/delivery count mismatch'
            assert s['txFail'] == 0 and self.status(i, 'PHY').get('abort') == 0, 'TX failure'
            assert self.status(i, 'WINDOW').get('logDrop') == 0, 'dropped serial logs'
            seqs = [r['sequence'] for r in self.records[i] if 'sequence' in r]
            assert seqs == list(range(s['delivered'])), 'incomplete, duplicated or reordered observed stream'
        self.phases.append(dict(name='exact_demo_streams_and_ACKs', passed=True, t=self.elapsed()))
        old = [self.status(i)['ack'] for i in range(2)]
        offset = [len(r) for r in self.records]
        messages = ['4FSK-A-012345', '4FSK-B-543210']
        for i, message in enumerate(messages):
            self.command(i, '/send ' + message)
        self.wait(lambda: all(any(r['hex'] == messages[1-i].encode().hex() for r in self.records[i][offset[i]:])
                              and self.status(i)['ack'] > old[i] for i in range(2)), 30, 'manual_payloads_exact_and_acked')
        rx = [self.status(i)['rx'] for i in range(2)]
        self.command_both('/recal')
        self.wait(lambda: self.locked() and all(self.status(i)['rx'] >= rx[i] + 3 for i in range(2)), 40, 'cold_calibration_reacquired')
        assert self.boots == expected_boots, 'unexpected reboot'
        assert not self.warnings, 'serial diagnostics incomplete; see warnings'
        assert not self.errors

    def finish(self, passed):
        for i in range(len(self.ports)):
            try:
                self.command(i, '/limit 5')
                self.command(i, '/auto on')
                self.command(i, '/stats')
            except Exception as exc:
                self.errors.append('restore defaults: ' + str(exc))
        end = time.monotonic() + 2
        while time.monotonic() < end:
            self.collect()
        result = dict(passed=passed and not self.errors, timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      elapsed=self.elapsed(), parameters=vars(self.args), phases=self.phases, latest=self.latest,
                      errors=self.errors, warnings=self.warnings, boots=self.boots,
                      independently_checked_deliveries=[len(r) for r in self.records], records=self.records, events=self.events,
                      final_requested_state=dict(limit=5, automatic_demo=True))
        Path(self.args.output).write_text(json.dumps(result, indent=2) + '\n')
        for port in self.ports:
            port.close()
        print('RESULT ' + json.dumps({k: result[k] for k in ('passed', 'elapsed', 'errors', 'boots', 'independently_checked_deliveries')}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
    parser.add_argument('--limit', type=int, default=5)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    bench = Bench(args)
    passed = False
    try:
        bench.run()
        passed = True
    except Exception as exc:
        bench.errors.append(str(exc))
        print('FAIL ' + str(exc), flush=True)
    finally:
        bench.finish(passed)
    raise SystemExit(0 if passed and not bench.errors else 1)
