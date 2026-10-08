#!/usr/bin/env python3
"""Validate the moved pair at its actual supported rate, not a forced peak."""
import argparse
from hardware_adaptive import AdaptiveBench


class DistanceBench(AdaptiveBench):
    def run(self):
        for board in range(2):
            self.command(board, '/reset')
        self.wait(lambda: all(self.sessions), 15, 'both_booted')
        self.wait(self.both_locked, 120, 'initial_lock_at_distance')
        baseline = [s.copy() for s in self.status]
        self.collect_for(self.args.seconds)
        assert not self.errors
        assert all(s.get('ack', 0) - old.get('ack', 0) >= 30
                   for s, old in zip(self.status, baseline)), 'insufficient acknowledged traffic'
        assert all(self.elapsed()-t < 15 for t in self.last_status_at), 'stale board status'
        assert all(s.get('up', 0) > 0 for s in self.status), 'no autonomous rate upgrade'
        assert all(a.get('waitMs', 999999) <= 60000 for a in self.adapt)
        self.phases.append(dict(name='distance_soak', passed=True, t=self.elapsed(),
                                baseline=baseline, status=[s.copy() for s in self.status]))
        self.command(1, '/limit 0')
        self.wait(lambda: self.both_locked() and all(s.get('rate') == 0 for s in self.status),
                  90, 'follower_cap_negotiated_over_rf')
        restored = self.elapsed()
        self.command(1, '/limit 5')
        self.wait(lambda: self.both_locked() and all(s.get('rate', 0) > 0 for s in self.status),
                  120, 'bounded_reacceleration')
        self.phases[-1]['duration'] = self.elapsed()-restored
        self.recovery_checks()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs=2, default=['/dev/ttyUSB1', '/dev/ttyUSB0'])
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--output', default='distance-validation.json')
    args = parser.parse_args()
    bench = DistanceBench(args)
    passed = False
    try:
        bench.run()
        passed = True
    except Exception as exc:
        bench.errors.append(str(exc))
        raise
    finally:
        bench.save(passed)
