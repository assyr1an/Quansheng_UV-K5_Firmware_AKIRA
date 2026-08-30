"""
Full 8KB EEPROM dump for the UV-K5.

Read-only: this tool cannot write to the radio (see k5_protocol.py header).

Usage:
    python k5_eeprom_dump.py --port COM22 --info
    python k5_eeprom_dump.py --port COM22 --radio k5-A
    python k5_eeprom_dump.py --verify k5-A-eeprom-full.raw k5-B-eeprom-full.raw

All three verification conditions must pass before ANY radio is written to.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import (  # noqa: E402
    K5Link, MAX_READ_CHUNK, EEPROM_SIZE, CALIBRATION_START, CALIBRATION_END,
)

BACKUP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "backups")


def require_serial():
    try:
        import serial
        return serial
    except ImportError:
        sys.exit("pyserial missing.  pip install pyserial")


def cmd_info(args):
    serial = require_serial()
    with K5Link(args.port, serial) as link:
        info = link.hello()
        print(f"  version            : {info['version']}")
        print(f"  custom AES key     : {info['has_custom_aes_key']}")
        print(f"  in lock screen     : {info['in_lock_screen']}")
        if info["has_custom_aes_key"] and info["in_lock_screen"]:
            print()
            print("  !! LOCKED. EEPROM reads will return ALL ZEROS with no error.")
            print("     Unlock the radio before dumping, or the backup is worthless.")
            return 1
        adc = link.read_adc()
        print(f"  RSSI               : {adc['rssi']}")
        print(f"  REG_65 (ex-noise)  : 0x{adc['ex_noise']:02X}  -> RNG bit {adc['rng_bit']}")
        print(f"  REG_63 (glitch)    : {adc['glitch']}")
    return 0


def cmd_dump(args):
    serial = require_serial()
    out_path = args.out or os.path.join(BACKUP_DIR, f"{args.radio}-eeprom-full.raw")
    out_path = os.path.abspath(out_path)

    if os.path.exists(out_path) and not args.force:
        sys.exit(f"refusing to overwrite existing backup: {out_path}\n(use --force)")

    with K5Link(args.port, serial) as link:
        info = link.hello()
        print(f"connected: {info['version']}")
        if info["has_custom_aes_key"] and info["in_lock_screen"]:
            sys.exit("radio is LOCKED — reads would silently return zeros. Aborting.")

        chunks = []
        for offset in range(0, EEPROM_SIZE, MAX_READ_CHUNK):
            chunks.append(link.read_eeprom(offset, MAX_READ_CHUNK))
            done = offset + MAX_READ_CHUNK
            print(f"\r  reading 0x{done:04X}/0x{EEPROM_SIZE:04X}", end="", flush=True)
        print()

    data = b"".join(chunks)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(data)
    print(f"wrote {len(data)} bytes -> {out_path}")

    ok = check_one(out_path, data)
    print()
    print("Copy this dump to a second drive before flashing anything.")
    return 0 if ok else 1


def check_one(path, data=None):
    """Runbook step 3 conditions 1 and 2."""
    if data is None:
        with open(path, "rb") as fh:
            data = fh.read()
    name = os.path.basename(path)
    ok = True

    if len(data) == EEPROM_SIZE:
        print(f"  [PASS] {name}: exactly {EEPROM_SIZE} bytes")
    else:
        print(f"  [FAIL] {name}: {len(data)} bytes, expected {EEPROM_SIZE}")
        ok = False

    calib = data[CALIBRATION_START:CALIBRATION_END]
    if not calib:
        print(f"  [FAIL] {name}: no calibration region present")
        return False
    if all(b == 0xFF for b in calib):
        print(f"  [FAIL] {name}: calibration 0x1E00-0x1FFF is ALL 0xFF — failed read")
        ok = False
    elif all(b == 0x00 for b in calib):
        print(f"  [FAIL] {name}: calibration 0x1E00-0x1FFF is ALL 0x00 — failed/locked read")
        ok = False
    else:
        distinct = len(set(calib))
        print(f"  [PASS] {name}: calibration looks real ({distinct} distinct byte values)")
    return ok


def cmd_verify(args):
    """Runbook step 3, all three conditions including the A-vs-B comparison."""
    print("Runbook step 3 verification")
    print()
    blobs = {}
    ok = True
    for path in args.files:
        path = os.path.abspath(path)
        if not os.path.exists(path):
            alt = os.path.join(BACKUP_DIR, os.path.basename(path))
            path = alt if os.path.exists(alt) else path
        if not os.path.exists(path):
            print(f"  [FAIL] missing file: {path}")
            ok = False
            continue
        with open(path, "rb") as fh:
            blobs[path] = fh.read()
        ok = check_one(path, blobs[path]) and ok

    paths = list(blobs)
    if len(paths) >= 2:
        print()
        if blobs[paths[0]] == blobs[paths[1]]:
            print("  [FAIL] the two dumps are IDENTICAL — same radio dumped twice,")
            print("         or the tool returned a cached buffer. Calibration is per-unit.")
            ok = False
        else:
            differing = sum(
                1 for a, b in zip(
                    blobs[paths[0]][CALIBRATION_START:CALIBRATION_END],
                    blobs[paths[1]][CALIBRATION_START:CALIBRATION_END],
                ) if a != b
            )
            print(f"  [PASS] dumps differ ({differing} differing calibration bytes)")
    elif len(paths) == 1:
        print()
        print("  [NOTE] only one file checked — the A-vs-B identity test needs both.")

    print()
    print("ALL CHECKS PASSED" if ok else "CHECKS FAILED — do not flash")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. COM22")
    ap.add_argument("--radio", help="radio label, e.g. k5-A (names the output file)")
    ap.add_argument("--out", help="explicit output path")
    ap.add_argument("--info", action="store_true", help="hello + ADC only, no dump")
    ap.add_argument("--force", action="store_true", help="overwrite an existing backup")
    ap.add_argument("--verify", nargs="+", dest="files", metavar="FILE",
                    help="verify existing dump(s) against the runbook conditions")
    args = ap.parse_args()

    if args.files:
        return cmd_verify(args)
    if not args.port:
        ap.error("--port is required (find it with: python k5_eeprom_dump.py --help)")
    if args.info:
        return cmd_info(args)
    if not args.radio and not args.out:
        ap.error("--radio (e.g. k5-A) or --out is required for a dump")
    return cmd_dump(args)


if __name__ == "__main__":
    sys.exit(main())
