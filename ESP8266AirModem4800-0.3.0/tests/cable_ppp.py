#!/usr/bin/env python3
"""Real Linux PPP over two simulated modem cores. Never opens a hardware port."""
import json
import os
from pathlib import Path
import pty
import subprocess
import sys
import time
import tty

def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=10).stdout

def main():
    if os.geteuid() != 0:
        raise SystemExit('Use sudo for isolated network namespaces; no hardware is opened.')
    root = Path(__file__).resolve().parents[1]
    result = dict(passed=False, scope='Linux pppd, PTYs, two C++ modem cores, simulated radio faults; NOT Windows or RF')
    namespaces, pairs, helpers, jobs = [], [], [], []
    sim = None
    start = time.monotonic()
    try:
        for suffix in ('a', 'b'):
            name = 'aircable-%d-%s' % (os.getpid(), suffix)
            run(['ip', 'netns', 'add', name]); namespaces.append(name)
            run(['ip', '-n', name, 'link', 'set', 'lo', 'up'])
            pair = pty.openpty(); pairs.append(pair); tty.setraw(pair[1])
        masters = [pair[0] for pair in pairs]
        sim = subprocess.Popen([str(root / 'validation/cable_sim')] + list(map(str, masters)),
                               pass_fds=masters, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for i in (1, 0):
            mode = '--cable-client' if i == 0 else '--cable-server'
            addresses = '192.0.2.1:192.0.2.2' if i == 0 else '192.0.2.2:192.0.2.1'
            command = ['ip', 'netns', 'exec', namespaces[i], sys.executable,
                       str(root / 'tools/modem_port.py'), '--port', os.ttyname(pairs[i][1]), mode,
                       '--ppp', 'noauth', 'nodefaultroute', 'noipv6', 'noccp', 'novj',
                       'lcp-restart', '5', 'ipcp-restart', '5', addresses]
            helpers.append(subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        deadline = time.monotonic() + 90
        while True:
            addresses = [json.loads(run(['ip', '-n', n, '-j', 'addr'])) for n in namespaces]
            if all(any(dev.get('ifname') == 'ppp0' and dev.get('addr_info') for dev in side) for side in addresses):
                break
            if time.monotonic() > deadline or any(p.poll() is not None for p in helpers):
                raise RuntimeError('PPP negotiation failed')
            time.sleep(.2)
        result['interfaces'] = addresses
        print('PASS IPCP over cable handshake and simulated radio', flush=True)
        ping = subprocess.Popen(['ip', 'netns', 'exec', namespaces[0], 'ping', '-n', '-c', '3', '-W', '15',
                                 '-i', '2', '192.0.2.2'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        jobs.append(ping)
        out, err = ping.communicate(timeout=60)
        result['ping'] = out.decode() + err.decode()
        if ping.returncode:
            raise RuntimeError(result['ping'])
        peers = str(root / 'tests/ppp_peer.py')
        server = subprocess.Popen(['ip', 'netns', 'exec', namespaces[1], sys.executable, peers, 'server'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        jobs.append(server); time.sleep(.3)
        client = subprocess.Popen(['ip', 'netns', 'exec', namespaces[0], sys.executable, peers, 'client'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        jobs.append(client)
        result['tcp'] = []
        for peer in (client, server):
            out, err = peer.communicate(timeout=200)
            if peer.returncode:
                raise RuntimeError(err.decode())
            result['tcp'].append(json.loads(out))
        result['passed'] = True
    except Exception as exc:
        result['error'] = repr(exc)
    finally:
        for process in jobs + helpers + ([sim] if sim else []):
            if process.poll() is None:
                process.terminate()
            try:
                out, err = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); out, err = process.communicate()
                result.setdefault('cleanup_errors', []).append('process needed SIGKILL')
            if process in helpers:
                result.setdefault('helper_logs', []).append(err.decode())
            if process is sim:
                result['simulation'] = err.decode()
                if process.returncode:
                    result['passed'] = False
        for pair in pairs:
            for fd in pair:
                os.close(fd)
        for namespace in reversed(namespaces):
            try:
                run(['ip', 'netns', 'delete', namespace])
            except Exception as exc:
                result.setdefault('cleanup_errors', []).append(repr(exc))
        result['seconds'] = time.monotonic() - start
        (root / 'validation/cable-ppp-simulation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result['passed'] and not result.get('cleanup_errors') else 1

if __name__ == '__main__':
    raise SystemExit(main())
