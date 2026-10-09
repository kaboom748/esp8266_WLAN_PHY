#!/usr/bin/env python3
"""Bounded startup check, not a complete RF/adaptation qualification."""
import argparse
from hardware_fsk4 import Bench

parser=argparse.ArgumentParser()
parser.add_argument('--ports',nargs=2,default=['/dev/ttyUSB1','/dev/ttyUSB0'])
parser.add_argument('--seconds',type=int,default=60)
parser.add_argument('--output',required=True)
args=parser.parse_args()
bench=Bench(args)
passed=False
try:
    bench.collect_for(1)
    old=bench.boots.copy()
    bench.command_both('/reset')
    bench.wait(lambda: all(a>b for a,b in zip(bench.boots,old)),20,'both_expected_4fsk_firmwares')
    expected_boots=bench.boots.copy()
    bench.command_both('/stats')
    bench.collect_for(args.seconds)
    criteria=dict(
        both_locked=bench.locked(),
        independent_payloads=all(len(r)>=5 for r in bench.records),
        acknowledgments=all(bench.status(i).get('ack',0)>=5 for i in range(2)),
        no_unexpected_restart=bench.boots==expected_boots,
        fixed_windows_without_filler=all(bench.status(i,'WINDOW').get('slotUs')==1000000 and
                                        bench.status(i,'WINDOW').get('fillBits')==0 for i in range(2)),
        no_tx_failure=all(bench.status(i).get('txFail')==0 and bench.status(i,'PHY').get('abort')==0 for i in range(2)),
    )
    passed=all(criteria.values()) and not bench.errors
    bench.phases.append(dict(name='bounded_default_configuration_smoke',passed=passed,
                             t=bench.elapsed(),criteria=criteria,latest=bench.snapshot(),
                             scope='Startup only. Stepwise adaptation is covered separately by host tests.'))
    print(('PASS ' if passed else 'FAIL ')+'bounded_default_configuration_smoke '+str(criteria),flush=True)
except Exception as exc:
    bench.errors.append(str(exc))
    print('FAIL '+str(exc),flush=True)
finally:
    bench.finish(passed)
raise SystemExit(0 if passed and not bench.errors else 1)
