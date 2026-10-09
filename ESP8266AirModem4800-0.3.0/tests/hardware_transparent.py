#!/usr/bin/env python3
"""USB/UART and actual RF validation. No host-side payload forwarding."""
import argparse
import hashlib
import json
import random
import time
from pathlib import Path
import serial


def escape(data):
    out = bytearray()
    for value in data:
        if value in (0x11, 0x13, 0x7d, 0x7e):
            out.extend((0x7d, value ^ 0x20))
        else:
            out.append(value)
    return bytes(out)


class Bench:
    def __init__(self, ports):
        self.names = ports
        self.ports = []
        self.data = [bytearray(), bytearray()]
        self.remote_paused = [False, False]
        self.ready = [True, True]
        self.last_control = [0.0, 0.0]
        self.xon = [0, 0]
        self.xoff = [0, 0]
        self.events = []
        self.started = time.monotonic()

    def event(self, kind, **fields):
        record = dict(t=round(time.monotonic() - self.started, 3), event=kind, **fields)
        self.events.append(record)
        print(json.dumps(record), flush=True)

    def open(self):
        for name in self.names:
            port = serial.Serial(baudrate=4800, timeout=0, write_timeout=.5,
                                 exclusive=True, xonxoff=False, rtscts=False, dsrdtr=False)
            port.dtr = False
            port.rts = False
            port.port = name
            port.open()
            self.ports.append(port)
        self.last_control = [0.0, 0.0]
        self.ready = [True, True]
        self.pump(3)

    def control(self, index, ready):
        self.ready[index] = ready
        self.ports[index].write(bytes([0x11 if ready else 0x13]))
        self.last_control[index] = time.monotonic()

    def pump(self, seconds=.02):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for i, port in enumerate(self.ports):
                if time.monotonic() - self.last_control[i] >= 1:
                    self.control(i, self.ready[i])
                for value in port.read(port.in_waiting or 1):
                    if value in (0x11, 0x13):
                        self.remote_paused[i] = value == 0x13
                        (self.xoff if value == 0x13 else self.xon)[i] += 1
                    else:
                        self.data[i].append(value)
            time.sleep(.002)

    def clear(self):
        self.data = [bytearray(), bytearray()]

    def transfer(self, payloads, timeout=40, pause=None):
        self.clear()
        sent = [0, 0]
        next_send = [0.0, 0.0]
        began = time.monotonic()
        next_progress = began + 10
        pending_pause = pause is not None
        if pause:
            self.control(pause[0], False)
            self.pump(.3)
        while time.monotonic() - began < timeout:
            now = time.monotonic()
            if pending_pause and now - began >= pause[1]:
                self.control(pause[0], True)
                pending_pause = False
                self.event('host_resumed', board='AB'[pause[0]])
            for i, port in enumerate(self.ports):
                if not self.remote_paused[i] and now >= next_send[i] and port.out_waiting < 32:
                    chunk = payloads[i][sent[i]:sent[i] + 16]
                    if chunk:
                        count = port.write(chunk)
                        sent[i] += count
                        next_send[i] = now + count / 480.0
            self.pump()
            for i in (0, 1):
                if not payloads[1-i].startswith(self.data[i]):
                    raise AssertionError('Unexpected/corrupted bytes on ' + 'AB'[i] + ': ' +
                                         repr(bytes(self.data[i][-80:])))
            if all(bytes(self.data[i]) == payloads[1-i] for i in (0, 1)):
                self.pump(.3)
                assert all(bytes(self.data[i]) == payloads[1-i] for i in (0, 1)), 'Late extra payload'
                return dict(seconds=round(time.monotonic() - began, 3),
                            sent=sent, received=[len(x) for x in self.data],
                            sha256=[hashlib.sha256(x).hexdigest() for x in payloads])
            if now >= next_progress:
                self.event('progress', sent=sent[:], received=[len(x) for x in self.data],
                           xoff=self.xoff[:])
                next_progress += 10
        raise AssertionError('Transfer timeout: sent=%r received=%r tails=%r' %
                             (sent, [len(x) for x in self.data], [bytes(x[-40:]) for x in self.data]))

    def handshake(self, caller):
        payloads = [b'', b'']
        payloads[caller] = b'CLIENT'
        self.transfer(payloads)
        assert not self.data[caller], 'ESP synthesized CLIENTSERVER'
        self.clear()
        self.pump(.7)
        assert not any(self.data), 'Unsolicited handshake response'
        payloads = [b'', b'']
        payloads[1-caller] = b'CLIENTSERVER'
        self.transfer(payloads)

    def run(self):
        self.open()
        self.event('startup', received=[bytes(x).hex() for x in self.data])
        self.clear()
        self.pump(1)
        assert not any(self.data), 'Unsolicited application text'
        self.transfer([b'FIRST-A-without-handshake', b'FIRST-B-without-handshake'])
        self.event('transparent_from_boot_pass')
        self.transfer([b'AT\rATH\rATZ\rATDT1\r+++CLIENTSERVER',
                       b'+++AT\rCLIENT\rATE0V1Q0\r'])
        self.event('at_and_escape_are_payload_pass')
        for caller in (0, 1, 0, 1, 0, 1):
            self.handshake(caller)
        self.event('repeated_handshakes_pass', count=6, local_responses=0)
        self.transfer([b'', b'CLIE'])
        self.control(0, False)
        self.pump(.3)
        self.clear()
        self.ports[1].write(b'NTSERVER')
        self.pump(4)
        assert not any(self.data), 'XOFF did not pause CLIENTSERVER'
        self.control(0, True)
        deadline = time.monotonic() + 15
        while bytes(self.data[0]) != b'NTSERVER' and time.monotonic() < deadline:
            self.pump()
        self.pump(.3)
        assert self.data == [bytearray(b'NTSERVER'), bytearray()], 'Resume changed CLIENTSERVER'
        self.event('xoff_mid_clientserver_pass', prefix='CLIE', suffix='NTSERVER', pause_seconds=4)
        self.clear()
        self.ports[0].write(b'CL\x13I\x11ENT')
        deadline = time.monotonic() + 15
        while bytes(self.data[1]) != b'CLIENT' and time.monotonic() < deadline:
            self.pump()
        self.pump(.3)
        assert self.data == [bytearray(), bytearray(b'CLIENT')], 'Controls corrupted CLIENT'
        self.event('interleaved_controls_pass')
        originals = [bytes(range(256)) + random.Random(3000+i).randbytes(3840) for i in (0, 1)]
        payloads = [escape(x) for x in originals]
        integrity = self.transfer(payloads, timeout=150, pause=(1, 8))
        self.event('bidirectional_integrity_pass', **integrity)
        self.control(0, False)
        self.control(1, False)
        self.pump(1)
        for port in self.ports:
            port.close()
        self.ports = []
        time.sleep(.5)
        self.clear()
        self.open()
        self.event('reopen_boot_bytes', received=[bytes(x).hex() for x in self.data])
        # The CH340 open/close sequence resets these boards. Boot ROM emits at
        # 74880 baud; record those bytes separately from the application stream.
        self.clear()
        self.pump(1)
        assert not any(self.data), 'Unexpected application bytes after boot'
        self.handshake(0)
        self.handshake(1)
        self.event('close_reopen_xon_handshakes_pass', count=2)
        return dict(integrity=integrity, handshakes=8, xon=self.xon, xoff=self.xoff)

    def close(self):
        for port in self.ports:
            try:
                port.write(b'\x11')
            finally:
                port.close()
        self.ports = []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    bench = Bench(args.ports)
    result = dict(passed=False, scope='Actual ESP RF + UART, Linux manual XON/XOFF over VirtualHere; NOT Windows RAS')
    try:
        result.update(bench.run())
        result['passed'] = True
    except Exception as exc:
        result['error'] = repr(exc)
        bench.event('failure', error=repr(exc), received=[len(x) for x in bench.data],
                    tails=[bytes(x[-80:]).hex() for x in bench.data])
    finally:
        try:
            bench.close()
        except Exception as exc:
            result['cleanup_error'] = repr(exc)
        result['events'] = bench.events
        Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['passed'] and 'cleanup_error' not in result else 1


if __name__ == '__main__':
    raise SystemExit(main())
