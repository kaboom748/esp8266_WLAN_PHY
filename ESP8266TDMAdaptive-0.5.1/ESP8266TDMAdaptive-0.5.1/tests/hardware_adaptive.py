#!/usr/bin/env python3
"""Exercise rate negotiation and recovery on the two real ESP8266 boards."""
import argparse
from hardware_tdm import Bench


class AdaptiveBench(Bench):
    def run(self):
        for board in range(2):
            self.command(board, '/reset')
        self.wait(lambda: all(self.sessions), 15, 'both_booted')
        self.wait(self.both_locked, 90, 'initial_lock')
        self.wait(lambda: self.both_locked() and all(s.get('rate', 0) >= 1 for s in self.status),
                  240, 'autonomous_acceleration')
        self.wait(lambda: all(s.get('ack', 0) >= self.args.cells for s in self.status),
                  self.args.cells * 8 + 90, 'bidirectional_data_soak')
        self.wait(lambda: self.both_locked() and all(s.get('rate', 0) >= 2 for s in self.status),
                  240, 'fast_before_peer_request')
        before_down = self.status[0]['down']
        requested_limit = min(s['rate'] for s in self.status) - 1
        self.command(1, '/limit ' + str(requested_limit))
        self.wait(lambda: self.both_locked() and all(s.get('rate', 99) <= requested_limit for s in self.status)
                  and self.status[0].get('down', 0) > before_down,
                  90, 'follower_requests_slower_rate_over_rf')
        self.command(1, '/limit 5')
        self.wait(lambda: self.both_locked() and all(s.get('rate', 0) > requested_limit for s in self.status),
                  1500, 'automatic_reacceleration')
        self.recovery_checks()

    def recovery_checks(self):
        for board in range(2):
            self.command(board, '/auto off')
        self.wait(lambda: all(s.get('queue') == 0 for s in self.status), 60, 'queues_drained')
        baseline = [s.copy() for s in self.status]
        self.collect_for(22)
        assert all(s['rx'] - old['rx'] >= 3 and s['delivered'] == old['delivered']
                   for s, old in zip(self.status, baseline))
        self.phases.append(dict(name='empty_cells_continue', passed=True, t=self.elapsed()))
        self.command(1, '/pause')
        self.wait(lambda: self.status[0].get('state') == 'SEARCH' and self.status[0].get('rate') == 0,
                  60, 'outage_falls_back_to_common_base_rate')
        offset = len(self.events)
        self.command(0, '/send retained')
        self.collect_for(10)
        assert self.status[0].get('queue') == 1
        self.command(1, '/run')
        self.wait(lambda: self.both_locked() and self.status[0].get('queue') == 0,
                  90, 'outage_recovery_queue_retained')
        assert any(e.get('board') == 1 and 'hex=72657461696e6564' in e.get('text', '')
                   for e in self.events[offset:])
        for board in (1, 0):
            previous = self.sessions[board]
            self.command(board, '/reset')
            self.wait(lambda: self.sessions[board] != previous, 15, 'reboot_' + str(board + 1))
            self.wait(self.both_locked, 90, 'relock_after_reboot_' + str(board + 1))
        before = [s['rx'] for s in self.status]
        for board in range(2):
            self.command(board, '/recal')
        self.wait(lambda: self.both_locked() and all(s.get('rx', 0) >= old + 4
                  for s, old in zip(self.status, before)), 90, 'cold_calibration_reacquired')
        for board in range(2):
            self.command(board, '/limit 5')
            self.command(board, '/auto on')
        self.collect_for(12)
        assert not self.errors
        assert self.boots == [2, 2], self.boots
        assert all(s.get('txFail', 0) == 0 for s in self.status)
        assert all(s.get('abort', 0) == 0 for s in self.phy)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
    parser.add_argument('--cells', type=int, default=100)
    parser.add_argument('--output', default='adaptive-validation.json')
    args = parser.parse_args()
    bench = AdaptiveBench(args)
    passed = False
    try:
        bench.run()
        passed = True
    except Exception as exc:
        bench.errors.append(str(exc))
        raise
    finally:
        bench.save(passed)
