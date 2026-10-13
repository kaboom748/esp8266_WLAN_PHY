"""Build the two isolated experimental sketches; flash only when requested."""
import argparse
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--name", required=True)
p.add_argument("--source")
p.add_argument("--fs", type=int, default=312500)
p.add_argument("--half-us", type=int, default=4500)
p.add_argument("--duration", type=int, default=40)
p.add_argument("--bench", type=int, default=0)
p.add_argument("--bits", type=int, choices=(1, 2), default=1)
p.add_argument("--end-report", type=int, default=0)
p.add_argument("--flash", action="store_true")
p.add_argument("--sps", type=int, choices=(2,4,8), default=2)
p.add_argument("--iq", type=int, default=0)
p.add_argument("--fault", type=int, default=0)
p.add_argument("--matched", type=int, default=0)
p.add_argument("--gain", type=int, choices=range(-1,7), default=0)
p.add_argument("--bb-gain", type=int, choices=range(8), default=0)
p.add_argument("--integer", type=int, choices=(0,1,2), default=2)
p.add_argument("--channel", type=int, choices=range(1,12), default=11)
p.add_argument("--forward-channel", type=int, choices=range(1,12), default=3)
p.add_argument("--sync-limit", type=int, default=4)
p.add_argument("--accum", type=int, default=63)
p.add_argument("--scan", type=int, default=0)
p.add_argument("--phase-diversity", type=int, choices=(0,1), default=0)
p.add_argument("--quiet-sdk", type=int, choices=(0,1), default=0)
a = p.parse_args()
if not re.fullmatch(r"[A-Za-z0-9_-]+", a.name):
    p.error("name must be a single directory name")
root = Path(__file__).resolve().parent
dest = root / a.name
dest.mkdir(exist_ok=True)

def run(command, log=None):
    print("RUN", " ".join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if log:
        log.write_text(result.stdout)
    print(result.stdout[-6000:], flush=True)
    result.check_returncode()

run(["python3", root / "generate.py", "--source",
     a.source or (root / "backend_seed.ino" if (root / "backend_seed.ino").exists() else
                  "/home/kaboom/airmodem_rev7_300bps/Master_REV7/Master_REV7.ino"),
     "--out", dest, "--fs", a.fs, "--half-us", a.half_us,
     "--bench", a.bench, "--duration", a.duration, "--bits", a.bits, "--end-report", a.end_report,
     "--sps",a.sps,"--iq",a.iq,"--fault",a.fault,"--matched",a.matched,"--gain",a.gain,"--integer",a.integer,
     "--channel",a.channel,"--forward-channel",a.forward_channel or a.channel,
     "--sync-limit",a.sync_limit,"--accum",a.accum,"--scan",a.scan,"--bb-gain",a.bb_gain,
     "--phase-diversity",a.phase_diversity,"--quiet-sdk",a.quiet_sdk])
run(["g++", "-std=c++17", "-O2", "-DAIRMODEM_DATA_FSK_MODE=2",
     "-DAIRMODEM_IF_TARGET_HZ=9000", root / "test_codec.cpp", "-o", root / "test_codec"])
run([root / "test_codec"])
run(["g++", "-std=c++17", "-O2", "-DAIRMODEM_DATA_FSK_MODE=2", "-DFAST_INTEGER=2",
     "-DAIRMODEM_IF_TARGET_HZ=9000", root / "test_codec.cpp", "-o", root / "test_codec_integer"])
run([root / "test_codec_integer"])
fqbn = "esp8266:esp8266:d1_mini:xtal=160,eesz=4M"
for role in "AB":
    run(["arduino-cli", "compile", "--fqbn", fqbn, "--build-path",
         dest / ("build_" + role), dest / ("Fast" + role)], dest / ("build_" + role + ".log"))
if a.flash:
    for role, port in (("A", "/dev/ttyUSB1"), ("B", "/dev/ttyUSB0")):
        run(["arduino-cli", "upload", "-p", port, "--fqbn", fqbn, "--input-dir",
             dest / ("build_" + role), dest / ("Fast" + role)], dest / ("flash_" + role + ".log"))
