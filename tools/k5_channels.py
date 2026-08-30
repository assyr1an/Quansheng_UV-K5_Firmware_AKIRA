"""
Channel plan writer — programs memory channels over UART from a JSON plan file.

Encoding traced to source:

  radio.c:227           memory channel record base = channel * 16
  radio.c:238-300       record layout (see CHANNEL RECORD below)
  board.c:727           attributes array at 0x0D60, one byte per channel
  misc.h:186-197        ChannelAttributes_t: band:4, compander:2, scanlist2:1, scanlist1:1
  dcs.c:27              CTCSS_Options[55], units of 0.1 Hz
  frequencies.c:68      gStepFrequencyTable[], units of 10 Hz
  frequencies.c:28      frequencyBandTable[7]
  radio.h:64-74         MODULATION_FM=0, MODULATION_AM=1, MODULATION_USB=2

CHANNEL RECORD (16 bytes at channel*16)
  0..3   RX frequency, uint32 LE, units of 10 Hz
  4..7   TX frequency, uint32 LE
  8      RX code index   (into CTCSS_Options / DCS_Options)
  9      TX code index
  10     code types      low nibble = RX, high nibble = TX
  11     low nibble = TX offset direction, high nibble = modulation
  12     bit0 reverse, bit1 bandwidth (0=wide 1=narrow), bits2-3 output power
  13     bit0 DTMF decode, bits1-3 PTT ID mode
  14     step index
  15     scrambler

    python k5_channels.py --plan channels.example.json --port COM22 --dry-run
    python k5_channels.py --plan my-plan.json --port COM22 --write

The plan file is JSON — see channels.example.json. Per channel:
  name      up to 10 chars (required)
  rx        RX frequency in MHz (required)
  tx        TX frequency in MHz (defaults to rx — simplex / listen-only)
  tone      CTCSS tone in Hz for TX, e.g. 82.5 (default none)
  mod       "FM" or "AM" (default FM)
  bw        "wide" or "narrow" (default narrow)
  power     "low", "mid" or "high" (default low)
  step      "12.5" or "25" kHz (default 12.5)
  scanlist  true/false — include in scan lists 1+2 (default true)
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import K5Link  # noqa: E402

ATTR_BASE = 0x0D60

# dcs.c:27 — the firmware's full CTCSS_Options[55] table, Hz -> index.
_CTCSS_TABLE = [
    67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5,
    94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
    131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9,
    171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5,
    203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1,
    55.0, 57.5, 60.0, 62.5, 65.0,   # non-standard values
]
CTCSS = {hz: i for i, hz in enumerate(_CTCSS_TABLE)}

CODE_OFF = 0
CODE_CTCSS = 1

MOD_FM, MOD_AM = 0, 1
STEP_12_5, STEP_25 = 4, 5          # frequencies.c:68
BW_WIDE, BW_NARROW = 0, 1
PWR_LOW, PWR_MID, PWR_HIGH = 0, 1, 2

# frequencies.c:28 — lower bounds, 10 Hz units. Band = highest whose lower <= f.
BAND_LOWER = [0, 10800000, 13700000, 17400000, 35000000, 40000000, 47000000]


def band_of(freq_10hz):
    for b in range(len(BAND_LOWER) - 1, -1, -1):
        if freq_10hz >= BAND_LOWER[b]:
            return b
    return 0


def mhz(v):
    """MHz -> the firmware's 10 Hz units."""
    return int(round(v * 100000))


class Ch:
    def __init__(self, name, rx, tx=None, tone=None, mod=MOD_FM,
                 bw=BW_NARROW, power=PWR_LOW, step=STEP_12_5, scanlist=True):
        self.name = name
        self.rx = mhz(rx)
        self.tx = mhz(tx if tx is not None else rx)
        self.tone = tone
        self.mod = mod
        self.bw = bw
        self.power = power
        self.step = step
        self.scanlist = scanlist

    def record(self):
        r = bytearray(16)
        r[0:4] = struct.pack("<I", self.rx)
        r[4:8] = struct.pack("<I", self.tx)

        # RX code stays OFF on every channel. This is a MONITORING radio:
        # tone squelch on receive would mute exactly the traffic we want to hear.
        rx_code, rx_type = 0, CODE_OFF
        if self.tone is not None:
            tx_code, tx_type = CTCSS[self.tone], CODE_CTCSS
        else:
            tx_code, tx_type = 0, CODE_OFF

        r[8] = rx_code
        r[9] = tx_code
        r[10] = (rx_type & 0x0F) | ((tx_type & 0x0F) << 4)
        r[11] = (0 & 0x0F) | ((self.mod & 0x0F) << 4)   # offset dir 0 = explicit TX freq
        r[12] = (0) | (self.bw << 1) | (self.power << 2)
        r[13] = 0
        r[14] = self.step
        r[15] = 0
        return bytes(r)

    def attribute(self):
        a = band_of(self.rx) & 0x0F
        a |= (0 << 4)                       # compander off
        if self.scanlist:
            a |= (1 << 6) | (1 << 7)        # scanlist1 + scanlist2
        return a


MODS = {"FM": MOD_FM, "AM": MOD_AM}
BWS = {"wide": BW_WIDE, "narrow": BW_NARROW}
POWERS = {"low": PWR_LOW, "mid": PWR_MID, "high": PWR_HIGH}
STEPS = {"12.5": STEP_12_5, "25": STEP_25}


def load_plan(path):
    with open(path) as fh:
        doc = json.load(fh)
    plan = []
    for i, c in enumerate(doc["channels"]):
        try:
            tone = c.get("tone")
            if tone is not None and tone not in CTCSS:
                sys.exit(f"channel {i+1} ({c.get('name')}): tone {tone} is not in the "
                         f"firmware's CTCSS table (dcs.c CTCSS_Options)")
            plan.append(Ch(
                name=c["name"][:10],
                rx=c["rx"],
                tx=c.get("tx"),
                tone=tone,
                mod=MODS[c.get("mod", "FM")],
                bw=BWS[c.get("bw", "narrow")],
                power=POWERS[c.get("power", "low")],
                step=STEPS[str(c.get("step", "12.5"))],
                scanlist=bool(c.get("scanlist", True)),
            ))
        except KeyError as e:
            sys.exit(f"channel {i+1}: missing or invalid field {e}")
    return plan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True, help="JSON plan file (see channels.example.json)")
    ap.add_argument("--port", help="serial port, e.g. COM22")
    ap.add_argument("--write", action="store_true", help="actually write to EEPROM")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, touch nothing")
    ap.add_argument("--start", type=int, default=0, help="first channel index (0 = CH1)")
    args = ap.parse_args()

    plan = load_plan(args.plan)

    print(f"CHANNEL PLAN — {args.plan}")
    print(f"{'CH':>3}  {'name':<10} {'RX MHz':>10} {'TX MHz':>10} {'tone':>6} "
          f"{'mod':>3} {'bw':>6} {'band':>4}")
    for i, c in enumerate(plan):
        print(f"{args.start+i+1:>3}  {c.name:<10} {c.rx/100000:>10.5f} {c.tx/100000:>10.5f} "
              f"{(str(c.tone) if c.tone else '-'):>6} "
              f"{'AM' if c.mod == MOD_AM else 'FM':>3} "
              f"{'narrow' if c.bw else 'wide':>6} {band_of(c.rx):>4}")

    if args.dry_run or not args.write:
        print("\ndry run — nothing written. Use --write to commit.")
        return 0

    if not args.port:
        sys.exit("--port is required with --write")

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing.  pip install pyserial")

    print()
    bad = 0
    with K5Link(args.port, serial) as link:
        info = link.hello()
        print(f"connected: {info['version']}")
        for i, c in enumerate(plan):
            ch = args.start + i
            rec = c.record()
            base = ch * 16
            ok = link.write_eeprom(base, rec[0:8]) and link.write_eeprom(base + 8, rec[8:16])
            ok = ok and link.write_eeprom_byte(ATTR_BASE + ch, c.attribute())
            print(f"  CH{ch+1:>3} {c.name:<10} {'ok' if ok else 'FAILED'}")
            if not ok:
                sys.exit("write failed — stopping")

        print("\nverifying by reading back...")
        for i, c in enumerate(plan):
            ch = args.start + i
            got = link.read_eeprom(ch * 16, 16)
            att = link.read_eeprom((ATTR_BASE + ch) & ~7, 8)[(ATTR_BASE + ch) & 7]
            if got != c.record() or att != c.attribute():
                print(f"  CH{ch+1} MISMATCH")
                print(f"    want {c.record().hex(' ')}  attr {c.attribute():02X}")
                print(f"    got  {got.hex(' ')}  attr {att:02X}")
                bad += 1
        print(f"  {len(plan)-bad}/{len(plan)} channels verified byte-exact")

    print("\nPower-cycle the radio for it to reload the channel list.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
