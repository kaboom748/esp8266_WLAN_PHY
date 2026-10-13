"""300 baud/8N1/no flow control hardware test, with loss-resilient framing.

Host USB timestamps are intentionally not presented as wire latency or RF BER.
Raw streams are retained so framing failures cannot be mistaken for bit flips.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import random
import struct
import threading
import time
import zlib
from pathlib import Path

import serial

END, ESC = 0xC0, 0xDB

def frame(body):
    raw = body + struct.pack("<I", zlib.crc32(body))
    return bytes([END]) + raw.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + bytes([END])

def parse(raw):
    frames, invalid = [], 0
    for segment in raw.split(bytes([END]))[1:-1]:
        if not segment:
            continue
        data, i, ok = bytearray(), 0, True
        while i < len(segment):
            b = segment[i]
            if b == ESC:
                i += 1
                if i >= len(segment) or segment[i] not in (0xDC, 0xDD):
                    ok = False
                    break
                b = END if segment[i] == 0xDC else ESC
            data.append(b)
            i += 1
        if (not ok or len(data) != 23 or
                zlib.crc32(data[:-4]) != struct.unpack("<I", data[-4:])[0]):
            invalid += 1
        else:
            frames.append(bytes(data[:-4]))
    return frames, invalid

def make_stream(role, count):
    rng = random.Random(0x300000 + role)
    bodies = [struct.pack("<BH", role, seq) +
              (bytes(range(seq * 16, (seq + 1) * 16)) if seq < 16 else rng.randbytes(16))
              for seq in range(count)]
    return b"".join(map(frame, bodies)), bodies

def summarize(sent, expected, received):
    decoded, invalid = parse(received)
    seen, mismatch, duplicate, reordered, last = set(), 0, 0, 0, -1
    for body in decoded:
        role, seq = struct.unpack("<BH", body[:3])
        if seq >= len(expected) or body != expected[seq]:
            mismatch += 1
            continue
        if seq in seen:
            duplicate += 1
        if seq < last:
            reordered += 1
        seen.add(seq)
        last = seq
    it = iter(sent)
    subsequence = all(any(v == wanted for v in it) for wanted in received)
    return dict(sent_bytes=len(sent), received_bytes=len(received),
                expected_frames=len(expected), exact_frames=len(seen),
                missing_frames=len(expected)-len(seen), invalid_frames=invalid,
                crc_valid_wrong_payload=mismatch, duplicates=duplicate,
                reordered=reordered, exact_stream=sent == received,
                received_is_subsequence_of_sent=subsequence,
                sha256_sent=hashlib.sha256(sent).hexdigest(),
                sha256_received=hashlib.sha256(received).hexdigest(),
                sent_hex=sent.hex(), received_hex=received.hex())

def open_port(path):
    port = serial.Serial(None, baudrate=300, bytesize=8, parity="N", stopbits=1,
                         timeout=0, write_timeout=120, xonxoff=False,
                         rtscts=False, dsrdtr=False, exclusive=True)
    port.dtr = False
    port.rts = False
    port.port = path
    port.open()
    return port

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--output", required=True)
    parser.add_argument("--telemetry-until", type=int, default=0)
    parser.add_argument("--drain", type=float, default=8)
    parser.add_argument("--firmware", default="unspecified")
    args = parser.parse_args()
    result = dict(baud=300, format="8N1", flow_control=False, firmware=args.firmware,
                  started_utc=datetime.now(timezone.utc).isoformat(), cases={})
    ports = [open_port("/dev/ttyUSB1"), open_port("/dev/ttyUSB0")]
    opened=time.monotonic()
    try:
        time.sleep(6)
        for name, active in (("A_to_B", (0,)), ("B_to_A", (1,)), ("simultaneous", (0, 1))):
            for port in ports:
                port.reset_input_buffer()
            streams = [make_stream(i + 1, args.frames) for i in range(2)]
            received = [bytearray(), bytearray()]
            errors = []
            barrier = threading.Barrier(len(active) + 1)
            def writer(i):
                try:
                    barrier.wait()
                    if ports[i].write(streams[i][0]) != len(streams[i][0]):
                        raise RuntimeError("short serial write")
                    ports[i].flush()
                except Exception as exc:
                    errors.append(repr(exc))
            threads = [threading.Thread(target=writer, args=(i,)) for i in active]
            for thread in threads:
                thread.start()
            barrier.wait()
            began = time.monotonic()
            duration = max(len(streams[i][0]) for i in active) / 30 + args.drain
            next_report = began + 10
            while time.monotonic() - began < duration or any(t.is_alive() for t in threads):
                for i, port in enumerate(ports):
                    received[i].extend(port.read(4096))
                if time.monotonic() >= next_report:
                    print(name, "elapsed", round(time.monotonic()-began, 1),
                          "received", list(map(len, received)), flush=True)
                    next_report += 10
                time.sleep(.002)
            for thread in threads:
                thread.join()
            case = dict(elapsed_seconds=time.monotonic()-began, host_errors=errors)
            for i in active:
                case["A_to_B" if i == 0 else "B_to_A"] = summarize(*streams[i], bytes(received[1-i]))
            if len(active) == 1:
                case["unexpected_reverse_bytes"] = len(received[active[0]])
            result["cases"][name] = case
            Path(args.output).write_text(json.dumps(result, indent=2))
            print(json.dumps({name: {key: {k: v for k, v in value.items() if not k.endswith("hex")}
                                    if isinstance(value, dict) else value for key, value in case.items()}}), flush=True)
            time.sleep(2)
        if args.telemetry_until:
            for port in ports:
                port.baudrate=115200
            telemetry=[bytearray(),bytearray()]
            while time.monotonic()-opened<args.telemetry_until:
                for i,port in enumerate(ports):
                    telemetry[i].extend(port.read(4096))
                time.sleep(.01)
            result["telemetry"]=[x.decode("utf-8",errors="replace") for x in telemetry]
            Path(args.output).write_text(json.dumps(result,indent=2))
            print(json.dumps({"telemetry":["\n".join(line for line in text.splitlines()
                  if not line.startswith("IQ ")) for text in result["telemetry"]]}),flush=True)
    finally:
        for port in ports:
            port.close()

if __name__ == "__main__":
    main()
