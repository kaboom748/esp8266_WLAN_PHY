#!/usr/bin/env python3
"""TCP integrity peer, run only inside the test network namespaces."""
import argparse
import hashlib
import json
import random
import socket

def payload(seed, size):
    return bytes(range(256)) + random.Random(seed).randbytes(size - 256)

def receive(sock, expected):
    got = bytearray()
    while len(got) < len(expected):
        data = sock.recv(len(expected) - len(got))
        if not data:
            raise EOFError('early TCP close')
        got.extend(data)
    assert got == expected, 'TCP content mismatch'
    return hashlib.sha256(got).hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['server', 'client'])
    p.add_argument('--size', type=int, default=4096)
    p.add_argument('--timeout', type=float, default=180)
    args = p.parse_args()
    assert 256 <= args.size <= 1048576
    listener = None
    sock = socket.socket()
    sock.settimeout(args.timeout)
    try:
        if args.mode == 'server':
            listener = sock
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(('192.0.2.2', 4800))
            listener.listen(1)
            sock, _ = listener.accept()
            sock.settimeout(args.timeout)
            digest = receive(sock, payload(1, args.size))
            sock.sendall(payload(2, args.size))
        else:
            sock.connect(('192.0.2.2', 4800))
            sock.sendall(payload(1, args.size))
            digest = receive(sock, payload(2, args.size))
        print(json.dumps(dict(mode=args.mode, received=args.size, sha256=digest)), flush=True)
    finally:
        sock.close()
        if listener:
            listener.close()

if __name__ == '__main__':
    main()
