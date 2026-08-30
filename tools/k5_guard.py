"""
Safety gate for any session that writes to the radio.

Run it BEFORE a write to establish a baseline, and AFTER every write to prove
nothing was collateral damage. It is read-only: it dumps EEPROM and compares,
and it can never write to the radio.

    python k5_guard.py --port COM22 --baseline <file.raw>            # check
    python k5_guard.py --port COM22 --baseline <file.raw> --save x.raw

Exit code 0 = safe to continue. Non-zero = STOP.

THE THREE GATES, in the order that matters:

 1. CALIBRATION (0x1E00-0x1FFF) BYTE-IDENTICAL.
    Per-radio factory RF calibration. Unrecoverable if lost - no backup any
    tool can make will restore performance, because CHIRP does not capture it
    and it is unique to this unit. This gate failing means stop everything.

 2. Only EXPECTED regions changed.
    Anything modified outside the ranges named with --allow is collateral
    damage, and the point of the gate is to notice it before it compounds.

 3. Radio still answers, and reports the version we expect.
    A radio that stopped answering mid-session is a different problem from one
    that answered wrongly, and the two need different responses.
"""

import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import K5Link  # noqa: E402

CAL_START = 0x1E00
CAL_END = 0x2000
EEPROM_SIZE = 0x2000


def parse_range(text):
    """'0x1D00-0x1D28' -> (0x1D00, 0x1D28), end exclusive."""
    lo, _, hi = text.partition("-")
    return int(lo, 0), int(hi, 0)


def read_all(link):
    data = bytearray()
    while len(data) < EEPROM_SIZE:
        data += link.read_eeprom(len(data), 128)
    return bytes(data)


def regions(diff_offsets):
    """Collapse a list of differing offsets into contiguous runs."""
    out = []
    for off in diff_offsets:
        if out and off == out[-1][1]:
            out[-1][1] = off + 1
        else:
            out.append([off, off + 1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baseline", required=True,
                    help="the .raw dump this state is compared against")
    ap.add_argument("--save", help="also write the fresh dump to this file")
    ap.add_argument("--allow", action="append", default=[],
                    help="a byte range that is EXPECTED to differ, e.g. "
                         "0x1D00-0x1D28. Repeatable.")
    ap.add_argument("--expect-version",
                    help="fail unless the reported version STARTS WITH this, e.g. AKIRA")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial is required:  pip install pyserial")
        return 2

    with open(args.baseline, "rb") as f:
        base = f.read()
    if len(base) != EEPROM_SIZE:
        print("BASELINE IS %d bytes, expected %d - refusing to compare"
              % (len(base), EEPROM_SIZE))
        return 2

    allowed = [parse_range(a) for a in args.allow]

    # ---- gate 3a: the radio answers at all --------------------------------
    try:
        with K5Link(args.port, serial) as link:
            info = link.hello()
            print("radio: %s" % info["version"])
            # Prefix match, so "AKIRA" passes for any build of ours while a
            # full string still pins one exactly if that is what you want.
            if args.expect_version and not info["version"].startswith(args.expect_version):
                print("GATE 3 FAIL: expected version starting %r" % args.expect_version)
                return 1
            now = read_all(link)
    except Exception as exc:            # noqa: BLE001 - any failure is a stop
        print("GATE 3 FAIL: radio did not answer - %s: %s"
              % (type(exc).__name__, exc))
        return 1

    if args.save:
        with open(args.save, "wb") as f:
            f.write(now)
        print("saved %d bytes -> %s" % (len(now), args.save))

    print("baseline sha256 %s" % hashlib.sha256(base).hexdigest()[:16])
    print("current  sha256 %s" % hashlib.sha256(now).hexdigest()[:16])

    failed = False

    # ---- gate 1: calibration ----------------------------------------------
    if now[CAL_START:CAL_END] == base[CAL_START:CAL_END]:
        print("[PASS] gate 1  calibration 0x1E00-0x1FFF byte-identical")
    else:
        n = sum(1 for i in range(CAL_START, CAL_END) if now[i] != base[i])
        print("[FAIL] gate 1  CALIBRATION CHANGED - %d bytes differ" % n)
        print("               STOP. This is per-radio and unrecoverable.")
        failed = True

    # ---- gate 2: only expected regions changed ----------------------------
    diffs = [i for i in range(EEPROM_SIZE) if now[i] != base[i]]
    unexpected = [i for i in diffs
                  if not any(lo <= i < hi for lo, hi in allowed)]

    if not diffs:
        print("[PASS] gate 2  nothing changed anywhere")
    else:
        for lo, hi in regions(list(diffs)):
            tag = "expected" if all(
                any(a <= i < b for a, b in allowed) for i in range(lo, hi)
            ) else "UNEXPECTED"
            print("       changed 0x%04X-0x%04X  (%d bytes)  %s"
                  % (lo, hi - 1, hi - lo, tag))
        if unexpected:
            print("[FAIL] gate 2  %d bytes changed outside the allowed ranges"
                  % len(unexpected))
            failed = True
        else:
            print("[PASS] gate 2  every change is inside an allowed range")

    print("\n%s" % ("STOP - a gate failed" if failed else "OK - safe to continue"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
