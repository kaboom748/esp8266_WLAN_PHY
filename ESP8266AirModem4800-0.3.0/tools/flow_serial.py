"""Bounded userspace XON/XOFF; independent of USB driver flow-control support."""
import time
import serial

class FlowSerial:
    LIMIT = 8192

    def __init__(self, port=None, device=None, clock=time.monotonic):
        self.clock = clock
        self.port = device or serial.Serial(baudrate=4800, timeout=0, write_timeout=0,
                                            xonxoff=False, rtscts=False, dsrdtr=False)
        if device is None:
            self.port.dtr = False
            self.port.rts = False
            self.port.port = port
            self.port.open()
        self.rx = bytearray()
        self.tx = bytearray()
        self.remote_paused = False
        self.local_paused = False
        self.control_due = True
        self.last_control = self.clock()
        self.next_write = self.clock()
        self.xoff_received = self.xon_received = 0

    def put(self, data):
        n = min(len(data), self.LIMIT - len(self.tx))
        self.tx.extend(data[:n])
        return n

    def read(self, size=None):
        n = len(self.rx) if size is None else min(size, len(self.rx))
        out = bytes(self.rx[:n])
        del self.rx[:n]
        return out

    def poll(self):
        now = self.clock()
        raw = self.port.read(min(1024, self.port.in_waiting or 1))
        for b in raw:
            if b == 0x13:
                self.remote_paused = True
                self.xoff_received += 1
            elif b == 0x11:
                self.remote_paused = False
                self.xon_received += 1
            else:
                if len(self.rx) == self.LIMIT:
                    raise BufferError('serial receive overflow; peer did not obey XOFF')
                self.rx.append(b)
        stop = self.local_paused
        if len(self.rx) >= 4096:
            stop = True
        elif len(self.rx) <= 1024:
            stop = False
        if stop != self.local_paused:
            self.local_paused = stop
            self.control_due = True
        if now - self.last_control >= 1:
            self.control_due = True
        if self.control_due:
            if self.port.write(b'\x13' if self.local_paused else b'\x11') == 1:
                self.last_control = now
                self.control_due = False
        # Do not trust out_waiting to include all bytes already in a USB bridge.
        if self.tx and not self.remote_paused and now >= self.next_write and self.port.out_waiting < 32:
            block = self.tx[:16]
            n = self.port.write(block)
            del self.tx[:n]
            self.next_write = now + n / 480.0

    def send(self, data, timeout=10):
        deadline = self.clock() + timeout
        pos = 0
        while pos < len(data) or self.tx:
            pos += self.put(data[pos:])
            self.poll()
            if self.clock() >= deadline:
                raise TimeoutError('serial transmit stalled')
            time.sleep(.002)

    def expect(self, marker, timeout=10):
        deadline = self.clock() + timeout
        while self.clock() < deadline:
            self.poll()
            at = self.rx.find(marker)
            if at >= 0:
                return self.read(at + len(marker))
            time.sleep(.002)
        raise TimeoutError('expected %r, got %r' % (marker, bytes(self.rx[-256:])))

    def close(self):
        self.port.close()
