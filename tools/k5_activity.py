"""
Read the radio's activity log — feature #4.

A record of which frequencies the SCANNER heard, and when. Read-only; the radio
does not clear the log on read, so this is safe to poll.

COVERAGE LIMIT, confirmed on hardware: the hook is CHFRSCANNER_Found(), which
only the channel/frequency scanner reaches. The spectrum analyser runs a
separate modal loop that never calls it — and that loop blocks the UART
entirely while its screen is open. This is "what the
scanner heard", never "everything the radio heard".

    python k5_activity.py --port COM22
    python k5_activity.py --port COM22 --csv activity.csv
"""

import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import K5Link  # noqa: E402

TICK_SECONDS = 1.28   # uptime >> 7, where a tick is 10ms


def ago(entry_ticks, now_ticks_16):
    """Entries carry uptime>>7 in 16 bits; the radio's 'now' is full 32-bit."""
    delta = (now_ticks_16 - entry_ticks) & 0xFFFF
    secs = int(delta * TICK_SECONDS)
    if secs < 60:
        return f"-{secs}s"
    if secs < 3600:
        return f"-{secs // 60}m{secs % 60:02d}s"
    return f"-{secs // 3600}h{(secs % 3600) // 60:02d}m"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--csv")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing.  pip install pyserial")

    with K5Link(args.port, serial) as link:
        print(f"connected: {link.hello()['version']}")
        log = link.read_activity_log()

    now16 = (log["uptime_ticks"] >> 7) & 0xFFFF
    up_s = log["uptime_ticks"] // 100
    print(f"radio uptime: {up_s // 3600}h{(up_s % 3600) // 60:02d}m{up_s % 60:02d}s")
    print(f"log: {log['count']} of {log['size']} entries used")
    print()

    if log["count"] == 0:
        print("  (empty — nothing has been heard during a scan since boot,")
        print("   or the log was wiped)")
        return 0

    print(f"  {'#':>3}  {'when':>8}  {'what':>14}  {'RSSI':>4}")
    rows = []
    for i, e in enumerate(log["entries"]):
        is_chan = bool(e["flags"] & 1)
        what = f"CH{e['freq_or_chan'] + 1}" if is_chan else f"{e['freq_or_chan'] / 100000:.5f}"
        when = ago(e["ticks"], now16)
        print(f"  {i:>3}  {when:>8}  {what:>14}  {e['rssi']:>4}")
        rows.append({"index": i, "when": when, "is_channel": is_chan,
                     "freq_or_chan": e["freq_or_chan"], "what": what,
                     "ticks": e["ticks"], "rssi": e["rssi"]})

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader(); w.writerows(rows)
        print(f"\nwrote {len(rows)} rows -> {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
