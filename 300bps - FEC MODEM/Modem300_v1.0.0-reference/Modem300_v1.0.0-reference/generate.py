"""Generate standalone Arduino sketches from the unchanged REV7 radio backend."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--source", required=True)
p.add_argument("--out", required=True)
p.add_argument("--fs", type=int, default=312500)
p.add_argument("--half-us", type=int, default=4500)
p.add_argument("--bench", type=int, default=0)
p.add_argument("--bits", type=int, choices=(1, 2), default=1)
p.add_argument("--end-report", type=int, default=0)
p.add_argument("--duration", type=int, default=40)
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
source = Path(a.source).read_text(encoding="utf-8-sig")
source = source[:source.index('// #line 1 "modemA_160k.ino"')]
assert source.count("SAMPLE_RATE = 30720") == 1
if not a.integer:
    source = source.replace("SAMPLE_RATE = 30720", f"SAMPLE_RATE = {a.fs}")
source = source.replace("int32_t phaseHz(", "int32_t IRAM_ATTR phaseHz(")
source = source.replace("int32_t demodulate(", "int32_t IRAM_ATTR demodulate(")
# The old capture scheduler is not instantiated. Its static assertions describe
# REV7's original sample rate, not the independent short-frame implementation.
source = source.replace("constexpr unsigned LOCKED_RX_SAMPLES = 2976;",
                        "constexpr unsigned LOCKED_RX_SAMPLES = 4096;")
here = Path(__file__).parent
backend = source[source.index("namespace AirModem {"):source.index("namespace AirModem {\n// Step 8")]
(here / "protocol_backend.h").write_text(
    '#include <cstdint>\n#include <cstring>\n#include <cstdlib>\n' + backend, encoding="ascii")
for role, name in ((1, "FastA"), (2, "FastB")):
    forward_channel = a.forward_channel or a.channel
    rx_channel, tx_channel = (a.channel, forward_channel) if role==1 else (forward_channel, a.channel)
    dest = Path(a.out) / name
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "legacy.h").write_text(source, encoding="ascii", errors="strict")
    (dest / "fast.h").write_bytes((here / "fast.h").read_bytes())
    (dest / f"{name}.ino").write_text(
        f"#define FAST_ROLE {role}\n#define FAST_FS {a.fs}\n"
        f"#define FAST_HALF_US {a.half_us}\n#define FAST_BENCH {a.bench}\n"
        f"#define FAST_BPS {a.bits}\n"
        f"#define FAST_SPS {a.sps}\n#define FAST_IQ {a.iq}\n"
        f"#define FAST_FAULT {a.fault}\n"
        f"#define FAST_MATCH {a.matched}\n"
        f"#define FAST_GAIN {a.gain}\n"
        f"#define FAST_BB_GAIN {a.bb_gain}\n"
        f"#define FAST_INTEGER {a.integer}\n"
        f"#define FAST_CHANNEL {rx_channel}\n#define FAST_TX_CHANNEL {tx_channel}\n"
        f"#define FAST_SYNC_LIMIT {a.sync_limit}\n"
        f"#define FAST_ACCUM {a.accum}\n"
        f"#define FAST_SCAN {a.scan}\n"
        f"#define FAST_PHASE_DIVERSITY {a.phase_diversity}\n"
        f"#define FAST_QUIET_SDK {a.quiet_sdk}\n"
        f"#define FAST_END_REPORT {a.end_report}\n"
        f"#define FAST_DURATION {a.duration}\n#include \"legacy.h\"\n"
        '#include "fast.h"\nFastModem application;\n'
        'void setup() { application.begin(); }\n'
        'void loop() { for(;;)application.poll(); }\n', encoding="ascii")
print(f"Generated {a.out}: fs={a.fs}, half={a.half_us}, bench={a.bench}")
