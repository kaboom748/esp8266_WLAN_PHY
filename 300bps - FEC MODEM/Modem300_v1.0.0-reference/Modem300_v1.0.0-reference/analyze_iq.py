"""Inspect raw diagnostic IQ without assigning an RF protocol to interference."""
import json
import re
import sys
import numpy as np

data = json.load(open(sys.argv[1]))
sync = np.array([-3 if (0xd3916ac5f027 >> (46-2*k)) & 2 else 3 for k in range(24)])
for text in data.get("telemetry", []):
    for line in text.splitlines():
        if not line.startswith("IQ "):
            if line.startswith(("FAST", "STAGES")):
                print(line)
            continue
        header, raw = line.rsplit(" ", 1)
        fields = dict(re.findall(r"(\w+)=(\w+)", header))
        fs, sps, count = [int(fields[k]) for k in ("fs", "sps", "n")]
        bps = int(fields.get("bps", 2))
        sync_at, symbols = 64//bps, 144//bps+24
        words = np.array([int(raw[k:k+8], 16) for k in range(0, len(raw), 8)], dtype=np.uint32)
        assert len(words) == count
        i = (words & 65535).astype(np.int16).astype(float)
        q = (words >> 16).astype(np.int16).astype(float)
        z = i-i.mean() + 1j*(q-q.mean())
        cross = z[1:]*z[:-1].conj()
        coarse = np.angle(cross.sum())*fs/(2*np.pi)
        hz = np.r_[0, np.angle(cross)*fs/(2*np.pi)]
        hz = (hz-coarse+fs/2) % fs - fs/2
        filtered = np.convolve(hz, np.ones(sps)/sps, "full")[:count]
        fits = []
        for start in range(sps*6, count-(symbols-1)*sps):
            y = filtered[start+(sync_at+np.arange(24))*sps]
            unit, offset = np.linalg.lstsq(np.c_[sync, np.ones(24)], y, rcond=None)[0]
            if abs(unit) < 100:
                continue
            metric = np.mean((y-offset-unit*sync)**2)/unit**2
            fits.append((metric, start, offset+coarse, unit))
        power = abs(z)**2
        report = dict(fields, dc_i=round(i.mean(), 2), dc_q=round(q.mean(), 2),
                      rms=round(np.sqrt(power.mean()), 2), peak=round(np.sqrt(power.max()), 2),
                      coarse_hz=round(coarse, 2), zero_iq=int(np.count_nonzero(words == 0)),
                      clipped=int(np.count_nonzero((abs(i)>=32767)|(abs(q)>=32767))),
                      best_sync=sorted(fits)[:3],
                      rms_blocks=[round(np.sqrt((abs(a)**2).mean()), 1) for a in np.array_split(z, 12)])
        print(json.dumps(report))
