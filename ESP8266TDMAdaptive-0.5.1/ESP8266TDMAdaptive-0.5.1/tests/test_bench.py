import types
import unittest
from hardware_tdm import Bench


class Port:
    def __init__(self, data):
        self.data = data.encode('ascii')

    @property
    def in_waiting(self):
        return len(self.data)

    def read(self, count):
        result, self.data = self.data[:count], self.data[count:]
        return result


class TelemetryTest(unittest.TestCase):
    def test_truncated_diagnostics_do_not_replace_last_complete_status(self):
        valid = ('TDM role=1 state=LOCKED cycle=10 tx=10 rx=10 miss=0 txFail=0 '
                 'ack=10 delivered=10 retry=0 dup=0 queue=0 resync=0 corr=0 '
                 'peerCorr=0 heap=48000 t=20000 rate=2 bitUs=1250 limit=5 '
                 'up=2 down=0 recovery=0\n')
        bench = Bench(types.SimpleNamespace(ports=[]))
        bench.ports = [Port(valid + 'TDM role=1 state=LOCKED cycle=11 ackn=0\n'),
                       Port(valid.replace('role=1', 'role=2'))]
        bench.collect()
        self.assertEqual(bench.status[0]['cycle'], 10)
        self.assertEqual(bench.status[0]['delivered'], 10)
        self.assertEqual(len(bench.warnings), 1)
        self.assertEqual(bench.errors, [])


if __name__ == '__main__':
    unittest.main()
