#!/usr/bin/env python3
"""Real pppd on two isolated network stacks, connected ONLY by UART and RF."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from flow_serial import FlowSerial

def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=10).stdout

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--transfer-timeout', type=float, default=180)
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB0', '/dev/ttyUSB1'], help='verified A and B ports')
    args = parser.parse_args()
    assert 256 <= args.size <= 1048576 and args.transfer_timeout > 0
    root = Path(__file__).resolve().parents[1]
    result = dict(passed=False, scope='IPCP and exact TCP payload; ICMP loss is reported separately', ppp_logs=['', ''])
    result['mode'] = 'transparent cable; Linux userspace flow control; NOT Windows RAS'
    result['ports'] = args.ports
    namespaces, modems, children, jobs = [], [], [], []
    began = time.monotonic()

    def pump():
        for i, modem in enumerate(modems):
            modem.poll()
            if not children:
                continue
            child = children[i]
            try:
                log = os.read(child.stderr.fileno(), 4096)
                result['ppp_logs'][i] += log.decode('utf-8', 'replace')
            except BlockingIOError:
                pass
            if child.poll() is not None:
                raise RuntimeError('pppd exited: ' + result['ppp_logs'][i])
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

    def until(condition, timeout, label):
        end = time.monotonic() + timeout
        while not condition():
            if time.monotonic() >= end:
                raise TimeoutError(label)
            pump()

    try:
        assert os.geteuid() == 0, 'run with sudo; namespaces isolate unauthenticated test PPP'
        for suffix in ('a', 'b'):
            name = 'airmodem-%d-%s' % (os.getpid(), suffix)
            run(['ip', 'netns', 'add', name])
            namespaces.append(name)
            run(['ip', '-n', name, 'link', 'set', 'lo', 'up'])
        for port in args.ports:
            modem = FlowSerial(port)
            modems.append(modem)
        boot_end = time.monotonic() + 3
        until(lambda: time.monotonic() >= boot_end, 5, 'boot settling')
        result['boot_bytes'] = [modem.read().hex() for modem in modems]
        modems[0].send(b'CLIENT')
        until(lambda: b'CLIENT' in modems[1].rx, 70, 'CLIENT through radio')
        assert b'CLIENTSERVER' not in modems[0].rx, 'modem fabricated a server response'
        modems[1].read(modems[1].rx.index(b'CLIENT') + 6)
        modems[1].send(b'CLIENTSERVER')
        until(lambda: b'CLIENTSERVER' in modems[0].rx, 10, 'server response through radio')
        modems[0].read(modems[0].rx.index(b'CLIENTSERVER') + 12)
        result['handshake'] = 'PC-generated CLIENT and CLIENTSERVER transported through both ESPs'
        print('RF connected; starting two isolated pppd instances', flush=True)
        for i, namespace in enumerate(namespaces):
            local, remote = ('192.0.2.1', '192.0.2.2') if i == 0 else ('192.0.2.2', '192.0.2.1')
            command = ['ip', 'netns', 'exec', namespace, '/usr/sbin/pppd',
                       'notty', 'nodetach', 'local', 'noauth', 'nocrtscts', 'logfd', '2',
                       'nodefaultroute', 'noipv6', 'noccp', 'novj',
                       'asyncmap', 'a0000', 'escape', '11,13', 'mtu', '296', 'mru', '296',
                       'lcp-restart', '5', 'lcp-max-configure', '20', 'ipcp-restart', '5',
                       local + ':' + remote]
            child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0)
            children.append(child)
            for stream in (child.stdin, child.stdout, child.stderr):
                os.set_blocking(stream.fileno(), False)
        until(lambda: all('remote IP address' in log for log in result['ppp_logs']), 90, 'IPCP negotiation')
        result['interfaces'] = [json.loads(run(['ip', '-n', n, '-j', 'addr', 'show', 'ppp0'])) for n in namespaces]
        print('PPP IPv4 up on both ESP radio endpoints', flush=True)
        ping = subprocess.Popen(['ip', 'netns', 'exec', namespaces[0], 'ping', '-n', '-c', '3', '-W', '15', '-i', '2', '192.0.2.2'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        jobs.append(ping)
        until(lambda: ping.poll() is not None, 60, 'ping')
        out, err = ping.communicate()
        result['ping'] = out.decode() + err.decode()
        result['ping_returncode'] = ping.returncode
        assert ping.returncode == 0, result['ping']
        script = str(root / 'tests' / 'ppp_peer.py')
        peer_options = ['--size', str(args.size), '--timeout', str(args.transfer_timeout)]
        server = subprocess.Popen(['ip', 'netns', 'exec', namespaces[1], sys.executable, script, 'server'] + peer_options,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        jobs.append(server)
        wait_until = time.monotonic() + .5
        until(lambda: time.monotonic() >= wait_until, 2, 'server start')
        client = subprocess.Popen(['ip', 'netns', 'exec', namespaces[0], sys.executable, script, 'client'] + peer_options,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        jobs.append(client)
        until(lambda: server.poll() is not None and client.poll() is not None, args.transfer_timeout + 20, 'TCP transfer')
        result['tcp'] = []
        for job in (server, client):
            out, err = job.communicate()
            assert job.returncode == 0, err.decode()
            result['tcp'].append(json.loads(out))
        result['flow'] = [dict(xoff=m.xoff_received, xon=m.xon_received) for m in modems]
        result['passed'] = True
        print('PASS PPP/TCP: %d bytes per direction, hashes checked; see separate ICMP loss statistics' % args.size, flush=True)
    except Exception as exc:
        result['error'] = repr(exc)
        print(repr(exc), flush=True)
    finally:
        for child in jobs + children:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        for m in modems:
            m.close()
        for n in reversed(namespaces):
            try:
                run(['ip', 'netns', 'delete', n])
            except Exception as exc:
                result.setdefault('cleanup_errors', []).append(str(exc))
        result['seconds'] = time.monotonic() - began
        Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(0 if result['passed'] and not result.get('cleanup_errors') else 1)

if __name__ == '__main__':
    main()
