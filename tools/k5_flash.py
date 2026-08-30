"""
UV-K5 firmware flasher.

Protocol taken from the canonical reference implementation, sq5bpf/k5prog
(k5prog.c), NOT reconstructed from memory:

  k5_obfuscate()                 framing: AB CD | len LE | data | crc16xmodem | DC BA,
                                 XOR-obfuscated over data+crc. Byte-identical to
                                 build_frame() in k5_protocol.py, which independently
                                 decoded the real bootloader beacon first try.
  k5_send_flash_version_message() 0x0530 session start
  k5_writeflash()                 0x0519 block write, 0x100 bytes per block
  reply                           0x051A, echoing the offset bytes

SAFETY
------
UVK5_MAX_FLASH_SIZE is 0xF000 because **the bootloader lives at 0xF000**. Writing
there is what actually bricks a radio beyond recovery. This tool refuses to write
at or above that address, unconditionally — there is no override flag.

Everything below 0xF000 is recoverable: the bootloader survives any failed flash,
and re-entry is PTT held while powering on (V1 board; white flashlight LED solid,
screen dark).

Flash the UNPACKED .bin, not the .packed.bin. The packed form is what the web
flasher consumes; it unpacks before sending. The version string goes in the
0x0530 message instead, and a leading '*' is accepted by all known bootloaders.

    python k5_flash.py --port COM22 --handshake-only
    python k5_flash.py --port COM22 --file <unpacked.bin> --yes
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import build_frame, parse_reply, FRAME_HEAD  # noqa: E402

BLOCK = 0x100
BOOTLOADER_ADDR = 0xF000       # k5prog UVK5_MAX_FLASH_SIZE — DO NOT WRITE AT/ABOVE
MIN_SANE_FIRMWARE = 50000      # k5prog's own failsafe against flashing a config file


def read_frame(ser, timeout=10.0):
    """Read one AB CD ... DC BA frame. Returns the de-obfuscated payload."""
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(512)
        if chunk:
            buf += chunk
        idx = buf.find(FRAME_HEAD)
        if idx < 0:
            if len(buf) > 1:
                del buf[:-1]
            continue
        if len(buf) < idx + 4:
            continue
        size = struct.unpack("<H", buf[idx + 2:idx + 4])[0]
        need = idx + 4 + size + 4
        if len(buf) >= need:
            return parse_reply(bytes(buf[idx:need]))
    return None


def wait_for_beacon(ser, timeout=15.0):
    """Bootloader announces itself with 0x0518 repeatedly while it waits."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        p = read_frame(ser, timeout=3.0)
        if p and len(p) >= 2 and struct.unpack("<H", p[0:2])[0] == 0x0518:
            ver = bytes(p[20:28]).split(b"\x00")[0].decode("ascii", "replace")
            return ver
    return None


def send_version(ser, version="*.01.23"):
    """0x0530 session start. k5prog: {0x30,0x05,0x10,0x00} + 8-byte version + zeros."""
    body = bytearray(20)
    body[0:4] = bytes([0x30, 0x05, 0x10, 0x00])
    v = version.encode("ascii")[:8]
    body[4:4 + len(v)] = v
    ser.write(build_frame(bytes(body)))
    return read_frame(ser, timeout=10.0)


def write_block(ser, data, offset, max_block_addr):
    """0x0519. Header is 16 bytes, then always a full 0x100 bytes of data."""
    if offset >= BOOTLOADER_ADDR:
        raise RuntimeError(f"REFUSING to write at 0x{offset:04X} — bootloader region")

    body = bytearray(16 + BLOCK)
    body[0] = 0x19
    body[1] = 0x05
    body[2] = 0x0C          # inner length 0x010C
    body[3] = 0x01
    body[4:8] = bytes([0x8A, 0x8D, 0x9F, 0x1D])
    body[8] = (offset >> 8) & 0xFF
    body[9] = offset & 0xFF
    body[10] = (max_block_addr >> 8) & 0xFF
    body[11] = 0x00
    body[12] = len(data) & 0xFF
    body[13] = (len(data) >> 8) & 0xFF
    body[14] = 0x00
    body[15] = 0x00
    body[16:16 + len(data)] = data      # remainder stays zero-filled

    ser.write(build_frame(bytes(body)))

    for _ in range(5):
        p = read_frame(ser, timeout=10.0)
        if not p:
            continue
        if len(p) >= 2 and struct.unpack("<H", p[0:2])[0] == 0x0518:
            continue                     # stray "still in flash mode" beacon
        if len(p) >= 10 and p[0] == 0x1A and p[8] == body[8] and p[9] == body[9]:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--file")
    ap.add_argument("--version", default="*.01.23")
    ap.add_argument("--handshake-only", action="store_true",
                    help="beacon + 0x0530 only. Writes NOTHING to flash.")
    ap.add_argument("--yes", action="store_true", help="required to actually write flash")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing.  pip install pyserial")

    ser = serial.Serial(args.port, 38400, timeout=0.3)

    print("waiting for bootloader beacon...")
    ver = wait_for_beacon(ser)
    if not ver:
        ser.close()
        sys.exit("no 0x0518 beacon — radio not in bootloader mode.\n"
                 "V1 board: hold PTT, switch on while holding, release.\n"
                 "Confirmed by a solid white flashlight LED and a dark screen.")
    print(f"  bootloader v{ver}")

    print(f"sending 0x0530 session start (version {args.version!r})...")
    reply = send_version(ser, args.version)
    if reply is None:
        ser.close()
        sys.exit("no reply to 0x0530 — handshake rejected, stopping before any write")
    rid = struct.unpack("<H", reply[0:2])[0] if len(reply) >= 2 else 0
    print(f"  reply 0x{rid:04X} ({len(reply)} bytes) — radio still talking")

    if args.handshake_only:
        ser.close()
        print("\nHANDSHAKE OK. Nothing was written to flash.")
        return 0

    if not args.file:
        ser.close()
        sys.exit("--file is required to flash")
    if not args.yes:
        ser.close()
        sys.exit("refusing to write flash without --yes")

    with open(args.file, "rb") as fh:
        fw = fh.read()

    if len(fw) < MIN_SANE_FIRMWARE:
        ser.close()
        sys.exit(f"{args.file} is only {len(fw)} bytes — too short to be firmware. Refusing.")
    if len(fw) > BOOTLOADER_ADDR:
        ser.close()
        sys.exit(f"{args.file} is 0x{len(fw):X} bytes, past the bootloader at 0x{BOOTLOADER_ADDR:X}. Refusing.")

    max_block_addr = (len(fw) & 0xFF00) + (BLOCK if (len(fw) & 0xFF) else 0)
    print(f"\nflashing {os.path.basename(args.file)}")
    print(f"  size 0x{len(fw):04X} ({len(fw)} bytes), blocks to 0x{max_block_addr:04X}")
    print(f"  bootloader at 0x{BOOTLOADER_ADDR:04X} will not be touched")
    print()

    ok = True
    for off in range(0, len(fw), BLOCK):
        chunk = fw[off:off + BLOCK]
        if not write_block(ser, chunk, off, max_block_addr):
            print(f"\n  *** NO CONFIRMATION at 0x{off:04X} — STOPPING ***")
            ok = False
            break
        pct = (off + len(chunk)) * 100 // len(fw)
        print(f"\r  0x{off:04X} / 0x{len(fw):04X}  {pct:3d}%", end="", flush=True)
    print()
    ser.close()

    if ok:
        print("\nFLASH COMPLETE. Power-cycle the radio normally (no buttons held).")
        return 0
    print("\nFLASH INCOMPLETE — the radio will not boot normal firmware until a")
    print("successful flash. The bootloader is untouched: hold PTT, switch on, retry.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
