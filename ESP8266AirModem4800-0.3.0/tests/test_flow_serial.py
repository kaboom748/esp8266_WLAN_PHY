import sys
import unittest
from unittest.mock import patch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from flow_serial import FlowSerial
from modem_port import cable_handshake

class Device:
    def __init__(self):
        self.incoming = bytearray()
        self.sent = bytearray()
        self.out_waiting = 0
    @property
    def in_waiting(self):
        return len(self.incoming)
    def read(self, n):
        out = bytes(self.incoming[:n]); del self.incoming[:n]; return out
    def write(self, data):
        self.sent.extend(data); return len(data)

class FlowTests(unittest.TestCase):
    def test_independent_directions_and_repeated_control(self):
        device = Device(); clock = [0.0]
        modem = FlowSerial(device=device, clock=lambda: clock[0])
        modem.put(b'payload'); device.incoming.extend(b'\x13received')
        modem.poll(); self.assertEqual(device.sent, b'\x11')
        self.assertEqual(modem.read(), b'received')
        clock[0] = 2; modem.poll(); self.assertEqual(device.sent, b'\x11\x11')
        device.incoming.extend(b'\x11'); modem.poll()
        self.assertTrue(device.sent.endswith(b'payload'))
        modem.rx.extend(b'x' * 4100); modem.poll(); self.assertEqual(device.sent[-1], 0x13)
        modem.read(); modem.poll(); self.assertEqual(device.sent[-1], 0x11)
    def test_bounds_and_pacing(self):
        device = Device(); clock = [0.0]
        modem = FlowSerial(device=device, clock=lambda: clock[0])
        self.assertEqual(modem.put(b'x' * 9000), 8192)
        modem.poll(); self.assertEqual(len(device.sent), 17)
        modem.poll(); self.assertEqual(len(device.sent), 17)
        clock[0] = 1 / 30; modem.poll(); self.assertEqual(len(device.sent), 33)

    def test_cable_server_preserves_first_ppp_bytes(self):
        device = Device()
        device.incoming.extend(b'noiseCLIENT\x7ePPP')
        modem = FlowSerial(device=device)
        cable_handshake(modem, False)
        self.assertEqual(modem.read(), b'\x7ePPP')
        self.assertEqual(device.sent, b'\x11CLIENTSERVER')

    def test_cable_client_retries_without_at_or_return(self):
        class Peer(Device):
            def write(self, data):
                n = super().write(data)
                if self.sent.count(b'CLIENT') == 2 and data == b'CLIENT':
                    self.incoming.extend(b'CLIENTSERVER\x7ePPP')
                return n
        device = Peer()
        clock = [0.0]
        def advance():
            clock[0] += .05
            return clock[0]
        modem = FlowSerial(device=device, clock=advance)
        with patch('modem_port.time.sleep'):
            cable_handshake(modem, True, timeout=10)
        self.assertEqual(bytes(b for b in device.sent if b not in (0x11, 0x13)), b'CLIENTCLIENT')
        self.assertEqual(modem.read(), b'\x7ePPP')

    def test_cable_client_timeout_and_flow_pause(self):
        device = Device(); clock = [0.0]
        def advance():
            clock[0] += .1
            return clock[0]
        modem = FlowSerial(device=device, clock=advance)
        device.incoming.extend(b'\x13')
        with patch('modem_port.time.sleep'), self.assertRaises(TimeoutError):
            cable_handshake(modem, True, timeout=3)
        self.assertNotIn(b'CLIENT', device.sent)

if __name__ == '__main__':
    unittest.main()
