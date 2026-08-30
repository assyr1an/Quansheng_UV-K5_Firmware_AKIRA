"""
Read and set the transmit-band policy (`F Lock`) and its sub-toggles over UART.

These live in the radio's menu, but `F Lock` is a HIDDEN menu item: the menu list
is truncated at it unless the radio was booted holding PTT + SIDE1
(helper/boot.c BOOT_GetMode -> BOOT_MODE_F_LOCK, main.c:122). This tool reaches
the same settings over the cable, so no boot gesture is needed.

    python k5_flock.py --port COM22 --show
    python k5_flock.py --port COM22 --set-lock 430
    python k5_flock.py --port COM22 --set-lock DEF --200tx on --500tx on

WHAT THESE ACTUALLY DO  (frequencies.c, TX_freq_check)
------------------------------------------------------
`F Lock` selects which frequencies the radio will TRANSMIT on. Receive is
unaffected and is already wide open in this firmware (18 MHz - 1300 MHz, minus
the BK4819's own 630-840 MHz hardware gap).

  DEF   137-174 and 400-470 always, plus whatever the sub-toggles below open
  FCC   144-148, 420-450
  CE    144-146, 430-440
  GB    144-148, 430-440
  430   137-174, 400-430
  438   137-174, 400-438
  PMR   446.00625-446.19375 only
  ALL   TX disabled on EVERY frequency - receive-only scanner mode.
        Note the name is the opposite of what it sounds like (settings.h:50).

Sub-toggles, which only have an effect under DEF:
  --200tx   174-350 MHz
  --350tx   350-400 MHz  (also needs --350en on)
  --350en   enables the 350-400 band at all
  --500tx   470-600 MHz

STORAGE - EEPROM 0x0F40, 8 bytes (settings.c:165-182, board.c:706-720)
  0 F_LOCK   1 350TX   2 (DTMF killed)   3 200TX
  4 500TX    5 350EN   6 ScrambleEnable  7 packed flags (AutoSv, battery text...)

Bytes 2, 6 and 7 are read and written back untouched, so this cannot disturb
the auto-store setting or anything else sharing the block.

READ-BACK VERIFIED. The driver's EEPROM write is fire-and-forget, so every write
here is read back and compared before the tool reports success.

Writes a configuration byte only. Nothing here keys the transmitter.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import K5Link  # noqa: E402

FLOCK_ADDR = 0x0F40
FLOCK_LEN = 8

# settings.h, enum F_LOCK order
LOCK_NAMES = ["DEF", "FCC", "CE", "GB", "430", "438", "PMR", "ALL"]

LOCK_HELP = {
    "DEF": "137-174 + 400-470, plus any sub-toggles below",
    "FCC": "144-148, 420-450",
    "CE":  "144-146, 430-440",
    "GB":  "144-148, 430-440",
    "430": "137-174, 400-430",
    "438": "137-174, 400-438",
    "PMR": "446.00625-446.19375 only",
    "ALL": "TX disabled everywhere - receive-only scanner mode",
}

# byte index in the block -> (flag name, default when the byte is erased)
TOGGLES = {
    "350tx": (1, False),
    "200tx": (3, False),
    "500tx": (4, False),
    "350en": (5, True),
}


def decode(block):
    raw = block[0]
    lock = LOCK_NAMES[raw] if raw < len(LOCK_NAMES) else "DEF (invalid %d, radio falls back)" % raw
    out = {"lock": lock}
    for name, (idx, default) in TOGGLES.items():
        v = block[idx]
        out[name] = bool(v) if v < 2 else default
    return out


def show(block):
    d = decode(block)
    print("  raw 0x0F40  " + " ".join("%02X" % b for b in block))
    print()
    print("  F Lock   %s" % d["lock"])
    key = d["lock"].split()[0]
    if key in LOCK_HELP:
        print("           -> %s" % LOCK_HELP[key])
    print()
    for name in ("200tx", "350tx", "350en", "500tx"):
        mark = "on " if d[name] else "off"
        note = "" if d["lock"] == "DEF" else "   (no effect unless F Lock = DEF)"
        print("  %-7s  %s%s" % (name, mark, note))
    return d


def main():
    ap = argparse.ArgumentParser(
        description="Read/set the TX band policy (F Lock) over UART.")
    ap.add_argument("--port", required=True, help="serial port, e.g. COM22 or /dev/ttyUSB0")
    ap.add_argument("--show", action="store_true", help="print the current settings and exit")
    ap.add_argument("--set-lock", metavar="MODE",
                    help="one of: " + ", ".join(LOCK_NAMES))
    for name in TOGGLES:
        ap.add_argument("--" + name, choices=["on", "off"],
                        help="only has an effect under F Lock = DEF")
    ap.add_argument("--yes", action="store_true", help="skip the confirmation")
    args = ap.parse_args()

    wants_write = args.set_lock is not None or any(
        getattr(args, n.replace("-", "_")) for n in TOGGLES)

    if args.set_lock is not None and args.set_lock.upper() not in LOCK_NAMES:
        sys.exit("unknown F Lock mode %r - choose from: %s"
                 % (args.set_lock, ", ".join(LOCK_NAMES)))

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing.  python -m pip install -r requirements.txt")

    with K5Link(args.port, serial) as link:
        info = link.hello()
        print("radio: %s\n" % info["version"])

        block = bytearray(link.read_eeprom(FLOCK_ADDR, FLOCK_LEN))
        print("current:")
        show(block)

        if not wants_write or args.show:
            return 0

        new = bytearray(block)
        if args.set_lock is not None:
            new[0] = LOCK_NAMES.index(args.set_lock.upper())
        for name, (idx, _default) in TOGGLES.items():
            val = getattr(args, name.replace("-", "_"))
            if val is not None:
                new[idx] = 1 if val == "on" else 0

        if new == block:
            print("\nnothing to change.")
            return 0

        print("\nnew:")
        show(new)
        print("\n  bytes 2, 6 and 7 are preserved unchanged"
              " (scrambler, auto-store and the packed flags).")

        if not args.yes:
            if input("\nwrite this to EEPROM? [y/N] ").strip().lower() != "y":
                print("aborted, nothing written")
                return 1

        if not link.write_eeprom(FLOCK_ADDR, bytes(new)):
            sys.exit("write FAILED - nothing verified, check the cable")

        read_back = bytearray(link.read_eeprom(FLOCK_ADDR, FLOCK_LEN))
        if read_back != new:
            print("\n  wrote %s" % " ".join("%02X" % b for b in new))
            print("  read  %s" % " ".join("%02X" % b for b in read_back))
            sys.exit("READ-BACK MISMATCH - the write did not land. Do not trust the radio's "
                     "TX policy until this is resolved.")

        print("\n  read back and verified.")

    print("\nPower-cycle the radio. These are loaded into RAM at boot"
          " (board.c:706), and a running radio would otherwise save its old"
          " values back over yours.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
