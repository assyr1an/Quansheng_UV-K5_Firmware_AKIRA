"""
Post-flash self-check — everything verifiable over the cable, without human eyes.

This covers the UART-reachable half of the post-flash smoke test, plus
several things a visual inspection genuinely cannot do: notably a byte-level
comparison of the whole EEPROM against the pre-flash backup, which is the only way
to prove the flash left factory RF calibration untouched.

READ-ONLY. Writes nothing to the radio.

    python k5_selftest.py --port COM22 --baseline backups/k5-A-eeprom-full.raw
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import (  # noqa: E402
    K5Link, MAX_READ_CHUNK, EEPROM_SIZE, CALIBRATION_START, CALIBRATION_END,
)

# Prefix, not an exact string. This constant went stale twice - once at 3647d2b
# and again at 3b2e461 - because it pinned a git hash that changes every build.
# A check that asserts the wrong expectation is worse than no check: it passes.
EXPECT_VERSION = "AKIRA"

results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    mark = "PASS" if ok else "FAIL" if ok is False else "NOTE"
    print(f"  [{mark}] {name}")
    if detail:
        for line in detail.splitlines():
            print(f"         {line}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baseline", help="pre-flash EEPROM dump to compare against")
    ap.add_argument("--expect-version", default=EXPECT_VERSION)
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing.  pip install pyserial")

    print("=" * 70)
    print("UV-K5 POST-FLASH SELF-CHECK  (UART-reachable checks only)")
    print("=" * 70)
    print()

    with K5Link(args.port, serial) as link:

        # ---------------------------------------------------------- identity
        print("1. FIRMWARE IDENTITY")
        info = link.hello()
        check(f"version reports {info['version']!r}",
              info["version"].startswith(args.expect_version),
              f"expected a version starting {args.expect_version!r}")
        check("radio is not locked",
              not (info["has_custom_aes_key"] and info["in_lock_screen"]),
              f"custom AES key={info['has_custom_aes_key']}  lock screen={info['in_lock_screen']}")
        print()

        # ------------------------------------------------------ command surface
        print("2. UART COMMAND SURFACE")
        try:
            adc = link.read_adc()
            check("CMD_0527 (RSSI / REG_65 / REG_63) responds", True,
                  f"RSSI={adc['rssi']}  REG_65=0x{adc['ex_noise']:02X}  REG_63={adc['glitch']}")
        except Exception as e:
            check("CMD_0527 responds", False, str(e))
        try:
            bat = link.read_battery()
            raw = bat["voltage_raw"]
            # board.c converts raw ADC to volts; sane li-ion pack lands well inside this
            plausible = 100 < raw < 3000
            check("CMD_0529 (battery) responds with a plausible reading", plausible,
                  f"raw ADC = {raw}  (firmware never populates Current - its own comment says so)")
        except Exception as e:
            check("CMD_0529 responds", False, str(e))
        try:
            probe = link.read_eeprom(0x0000, 16)
            check("CMD_051B (EEPROM read) responds", len(probe) == 16,
                  f"first 16 bytes: {probe.hex(' ')}")
        except Exception as e:
            check("CMD_051B responds", False, str(e))
        print()

        # ------------------------------------------------- ADC actually alive
        print("3. RECEIVER FRONT-END IS LIVE (not a stuck register)")
        samples = []
        t0 = time.perf_counter()
        while len(samples) < 240 and time.perf_counter() - t0 < 20:
            samples.extend(link.read_adc_batch(8))
        rssi = [s["rssi"] for s in samples]
        reg = [s["ex_noise"] for s in samples]
        check("RSSI varies (front-end is sampling, not frozen)",
              len(set(rssi)) > 1,
              f"{len(set(rssi))} distinct values over {len(samples)} samples, "
              f"range {min(rssi)}-{max(rssi)}")
        check("REG_65 varies", len(set(reg)) > 1,
              f"{len(set(reg))} distinct values, range {min(reg)}-{max(reg)}")
        print()

        # -------------------------------------------------------- full EEPROM
        print("4. EEPROM INTEGRITY AFTER FLASH")
        chunks = []
        for off in range(0, EEPROM_SIZE, MAX_READ_CHUNK):
            chunks.append(link.read_eeprom(off, MAX_READ_CHUNK))
        live = b"".join(chunks)
        check(f"full {EEPROM_SIZE}-byte EEPROM reads back", len(live) == EEPROM_SIZE)

        calib = live[CALIBRATION_START:CALIBRATION_END]
        check("calibration region is not blank",
              not all(b == 0xFF for b in calib) and not all(b == 0x00 for b in calib),
              f"{len(set(calib))} distinct byte values in 0x1E00-0x1FFF")

    # -------------------------------------------- compare against pre-flash
    if args.baseline and os.path.exists(args.baseline):
        with open(args.baseline, "rb") as fh:
            base = fh.read()
        print()
        print("5. COMPARISON AGAINST THE PRE-FLASH BACKUP")
        print("   (this is the check human eyes cannot perform)")

        base_cal = base[CALIBRATION_START:CALIBRATION_END]
        check("factory RF calibration is BYTE-IDENTICAL to before the flash",
              base_cal == calib,
              "0x1E00-0x1FFF unchanged - the flash did not touch calibration"
              if base_cal == calib else
              f"*** {sum(1 for a,b in zip(base_cal,calib) if a!=b)} BYTES DIFFER - "
              "restore from the backup ***")

        diffs = [i for i, (a, b) in enumerate(zip(base, live)) if a != b]
        if not diffs:
            check("EEPROM entirely unchanged by the flash", True)
        else:
            lo, hi = min(diffs), max(diffs)
            regions = {}
            for i in diffs:
                regions.setdefault(i & 0xFFF0, 0)
                regions[i & 0xFFF0] += 1
            top = sorted(regions.items(), key=lambda kv: -kv[1])[:8]
            check(f"{len(diffs)} EEPROM bytes changed (settings region only?)",
                  hi < CALIBRATION_START,
                  f"range 0x{lo:04X}-0x{hi:04X}\n" +
                  "\n".join(f"0x{a:04X}: {c} byte(s)" for a, c in top) +
                  ("\nall below 0x1E00 - settings only, calibration untouched"
                   if hi < CALIBRATION_START else
                   "\n*** CHANGES AT OR ABOVE 0x1E00 - CALIBRATION AFFECTED ***"))

        # channels
        import struct
        att = live[0x0D60:0x0D60 + 200]
        prog = 0
        for ch in range(200):
            if att[ch] == 0xFF:
                continue
            rx = struct.unpack("<I", live[ch * 16:ch * 16 + 4])[0]
            if rx not in (0, 0xFFFFFFFF):
                prog += 1
        base_att = base[0x0D60:0x0D60 + 200]
        base_prog = 0
        for ch in range(200):
            if base_att[ch] == 0xFF:
                continue
            rx = struct.unpack("<I", base[ch * 16:ch * 16 + 4])[0]
            if rx not in (0, 0xFFFFFFFF):
                base_prog += 1
        check(f"channel memories preserved ({prog} programmed)",
              prog == base_prog, f"was {base_prog} before the flash")

        # our custom-config markers
        print()
        print("6. EEPROM MARKERS FOR OUR BUILD")
        check("0x0EA3 (MessengerConfig) readable",
              True, f"0x{live[0x0EA3]:02X} = 0b{live[0x0EA3]:08b}  "
                    f"(bit3 unused, bits6-7 unused2 -> 3 spare bits)")
        check("0x0F1F (spare byte reserved for feature #5) still 0xFF",
              live[0x0F1F] == 0xFF, f"0x{live[0x0F1F]:02X}")

    print()
    print("=" * 70)
    passed = sum(1 for _, ok in results if ok is True)
    failed = sum(1 for _, ok in results if ok is False)
    print(f"  {passed} passed, {failed} failed")
    print()
    print("  NOT covered here - these need eyes on the radio:")
    print("    boot screen · spectrum sweeps and EXITS · messenger T9 entry")
    print("    channel scan stops on signal · squelch opens/closes")
    print("    menu: FM radio / VOX / power-on password ABSENT")
    print("    menu: AirCopy ABSENT (Phase 0), EncKey + MsgEnc ABSENT (v2)")
    print("    menu: MsgRty present; KeyID matching the provisioning printout")
    print("=" * 70)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
