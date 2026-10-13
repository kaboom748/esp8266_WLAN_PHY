"""Summarize preserved bench records without converting erasures into a BER."""
import argparse
import json
import re
from pathlib import Path


def summarize(path):
    record = json.loads(path.read_text(encoding="utf-8"))
    rows = []
    for case_name, case in record["cases"].items():
        for direction in ("A_to_B", "B_to_A"):
            if direction not in case:
                continue
            row = case[direction]
            rows.append(dict(case=case_name, direction=direction,
                             sent_bytes=row["sent_bytes"], received_bytes=row["received_bytes"],
                             net_missing_bytes=row["sent_bytes"]-row["received_bytes"],
                             exact_stream=row["exact_stream"],
                             received_is_subsequence_of_sent=row["received_is_subsequence_of_sent"],
                             exact_frames=row["exact_frames"], invalid_frames=row["invalid_frames"],
                             crc_valid_wrong_payload=row["crc_valid_wrong_payload"],
                             duplicates=row["duplicates"], reordered=row["reordered"],
                             host_errors=case["host_errors"]))
    counters = {}
    for text in record.get("telemetry", []):
        for line in text.splitlines():
            kind = line.split(" ", 1)[0]
            if kind not in ("FAST_FINAL", "STAGES", "ARQ", "UART", "UARTIN", "ADC", "WIFI", "CLOCK"):
                continue
            fields = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
            if "r" in fields:
                role = str(fields.pop("r"))
                counters.setdefault(role, {})[kind] = fields
    return dict(file=path.name, firmware=record.get("firmware", "unspecified"),
                started_utc=record.get("started_utc"), rows=rows, counters=counters)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    records = [summarize(Path(path)) for path in args.files]
    output = dict(
        notes=["Fault-injection runs are not normal delivery-rate measurements.",
               "Missing-byte counts are length differences, not an RF bit-error rate.",
               "A subsequence is compatible with deletion; it does not prove absence of bit flips.",
               "Only exact_stream=true establishes a bit-for-bit match for the entire tested stream.",
               "Missing telemetry fields are unavailable, not zero.",
               "USB/VM timestamps do not certify first-bit wire latency."],
        records=records)
    Path(args.output).write_text(json.dumps(output, indent=2), encoding="utf-8")
    for record in records:
        print(record["file"])
        for row in record["rows"]:
            print(f"  {row['case']} {row['direction']}: {row['received_bytes']}/{row['sent_bytes']}; "
                  f"exact={row['exact_stream']}")


if __name__ == "__main__":
    main()
