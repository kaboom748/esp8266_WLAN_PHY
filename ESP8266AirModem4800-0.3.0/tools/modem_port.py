#!/usr/bin/env python3
"""Linux PPP over the boot-transparent radio cable."""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time
from flow_serial import FlowSerial

def cable_handshake(modem, client, timeout=90):
    if not client:
        modem.expect(b'CLIENT', timeout)
        modem.send(b'CLIENTSERVER', timeout)
        return
    deadline = modem.clock() + timeout
    retry = modem.clock()
    while modem.clock() < deadline:
        modem.poll()
        at = modem.rx.find(b'CLIENTSERVER')
        if at >= 0:
            modem.read(at + len(b'CLIENTSERVER'))
            return
        if modem.clock() >= retry and not modem.tx and not modem.remote_paused:
            modem.put(b'CLIENT')
            retry = modem.clock() + 2
        time.sleep(.002)
    raise TimeoutError('cable peer did not answer CLIENTSERVER')

def pump_child(modem, child):
    os.set_blocking(child.stdout.fileno(), False)
    os.set_blocking(child.stdin.fileno(), False)
    while child.poll() is None:
        modem.poll()
        if len(modem.tx) < 4096:
            try:
                data = os.read(child.stdout.fileno(), min(1024, 4096 - len(modem.tx)))
                assert modem.put(data) == len(data)
            except BlockingIOError:
                pass
        if modem.rx:
            try:
                n = os.write(child.stdin.fileno(), modem.rx[:1024])
                del modem.rx[:n]
            except BlockingIOError:
                pass
        time.sleep(.002)
    return child.returncode

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--cable-client', action='store_true', help='send CLIENT, then PPP; no AT initialization')
    mode.add_argument('--cable-server', action='store_true', help='answer CLIENT with CLIENTSERVER, then PPP')
    p.add_argument('--ppp', nargs=argparse.REMAINDER, help='pppd options, must be last; Linux only')
    args = p.parse_args()
    if args.ppp is None or os.name != 'posix':
        p.error('connection modes require --ppp on Linux')
    modem = FlowSerial(args.port)
    child = None
    try:
        signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
        cable_handshake(modem, args.cable_client)
        executable = shutil.which('pppd') or '/usr/sbin/pppd'
        options = [executable, 'notty', 'nodetach', 'local', 'nocrtscts', 'logfd', '2',
                   'asyncmap', 'a0000', 'escape', '11,13', 'mtu', '296', 'mru', '296',
                   'lcp-echo-interval', '10', 'lcp-echo-failure', '4']
        child = subprocess.Popen(options + args.ppp, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        raise SystemExit(pump_child(modem, child))
    finally:
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        modem.close()

if __name__ == '__main__':
    main()
