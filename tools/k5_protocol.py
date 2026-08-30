"""
Quansheng UV-K5 UART protocol — host side.

Every constant here is traced to the firmware source at commit 3647d2b:

  app/uart.c:143      Obfuscation[16] table
  app/uart.c:130-190  SendReply()  -> reply framing
  app/uart.c:400-478  parser       -> command framing, CRC, obfuscation switch
  app/uart.c:212-227  CMD_0514     -> hello, sets the session Timestamp
  app/uart.c:230-257  CMD_051B     -> EEPROM read (reply 0x051C)
  app/uart.c:306-317  CMD_0527     -> RSSI / REG_65 / REG_63 (reply 0x0528)
  driver/crc.c:23-33  CRC_16_CCITT, IV=0, no reversal, no inversion => CRC-16/XMODEM
  driver/uart.c:48    UART1->BAUD = Frequency / 39053  => 38400 8N1

WRITE POLICY
------------
EEPROM write (0x051D) was added 2026-08-27, deliberately and for one purpose: to
program the channel plan. It is guarded twice --
  * the firmware refuses any write at or above EEPROM_WRITE_MAX_ADDR 0x1E00
    (driver/eeprom.c:56, and CMD_051D always passes safe=true, app/uart.c:295)
  * write_eeprom() below refuses the same range host-side, before sending

Still NOT implemented, deliberately:
  0x05DD  reboot / bootloader
  anything that could key the transmitter (project rule: no remote TX)
"""

import struct
import time

BAUD = 38400

# app/uart.c:143
OBFUSCATION = bytes([
    0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
    0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80,
])

FRAME_HEAD = b"\xAB\xCD"
FRAME_TAIL = b"\xDC\xBA"

# app/uart.c:88-93 — REPLY_051B_t.Data.Data[128]
MAX_READ_CHUNK = 128

EEPROM_SIZE = 0x2000          # 8192 bytes
CALIBRATION_START = 0x1E00    # per-radio factory RF calibration — unrecoverable
CALIBRATION_END = 0x2000


def xor_obfuscate(data: bytes, start: int = 0) -> bytes:
    """app/uart.c:170-172 — index is absolute within the payload, not per-call."""
    return bytes(b ^ OBFUSCATION[(start + i) % 16] for i, b in enumerate(data))


def crc16_xmodem(data: bytes) -> int:
    """driver/crc.c — CRC-16/CCITT, init 0x0000, poly 0x1021, no reflection."""
    crc = 0x0000
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(payload: bytes, obfuscated: bool = True) -> bytes:
    """
    Command frame (app/uart.c:415-440):
        AB CD | size(uint16 LE) | [payload || crc16(payload)] | DC BA

    `size` counts the payload only; the parser reads CRC at Buffer[Size].
    The obfuscation covers payload+CRC (Size+2 bytes) — app/uart.c:470-474.

    Note on the obfuscation switch (app/uart.c:462-467): the firmware inspects the
    RAW header ID *before* de-obfuscating. An obfuscated hello (real ID 0x0514)
    appears raw as 0x6902 — 0x14^0x16=0x02, 0x05^0x6C=0x69 — which is exactly the
    constant that sets bIsEncrypted=True. So obfuscating everything is self-consistent
    and is what CHIRP does. Sending a *plaintext* hello instead pins the whole session
    to plaintext. We use the obfuscated path throughout.
    """
    body = payload + struct.pack("<H", crc16_xmodem(payload))
    if obfuscated:
        body = xor_obfuscate(body)
    return FRAME_HEAD + struct.pack("<H", len(payload)) + body + FRAME_TAIL


def parse_reply(raw: bytes, obfuscated: bool = True) -> bytes:
    """
    Reply frame (app/uart.c:130-190 SendReply):
        AB CD | size(uint16 LE) | payload(obfuscated) | 2 padding | DC BA

    Replies carry NO CRC — the two bytes before the tail are padding
    (Obfuscation[Size]^0xFF, Obfuscation[Size+1]^0xFF).
    """
    if not raw.startswith(FRAME_HEAD):
        raise ValueError(f"bad reply header: {raw[:8].hex(' ')}")
    size = struct.unpack("<H", raw[2:4])[0]
    if len(raw) < size + 8:
        raise ValueError(f"short reply: got {len(raw)} bytes, need {size + 8}")
    if raw[4 + size + 2: 4 + size + 4] != FRAME_TAIL:
        raise ValueError("bad reply footer — frame desync")
    payload = raw[4:4 + size]
    return xor_obfuscate(payload) if obfuscated else payload


class K5Link:
    """A read-only session with a UV-K5 over the programming cable."""

    def __init__(self, port, serial_module, timeout=1.0):
        self.ser = serial_module.Serial(port, BAUD, timeout=timeout)
        self.timestamp = 0

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read_frame(self, expect_size: int = None) -> bytes:
        """
        Resync on AB CD, then return exactly one frame.

        Reads in BULK rather than byte-at-a-time. This matters more than it
        looks: the FTDI driver's latency timer defaults to 16ms on Windows, so
        every separate read() that has to wait for the driver to flush costs a
        full 16ms. Byte-at-a-time reading paid that twice per command and
        capped throughput at ~40 samples/s. Reading the whole expected frame in
        one call pays it once.

        `expect_size` is the total frame length when known (16 for CMD_0527),
        which lets the first read ask for the whole thing at once.
        """
        deadline = time.time() + 3.0
        buf = bytearray()
        want = expect_size or 1
        while time.time() < deadline:
            chunk = self.ser.read(max(want - len(buf), 1))
            if chunk:
                buf += chunk
            elif not buf:
                continue

            idx = buf.find(FRAME_HEAD)
            if idx < 0:
                # keep only a trailing byte in case AB straddles two reads
                if len(buf) > 1:
                    del buf[:-1]
                continue
            if len(buf) < idx + 4:
                want = idx + 4
                continue
            size = struct.unpack("<H", buf[idx + 2:idx + 4])[0]
            need = idx + 4 + size + 4
            if len(buf) >= need:
                return bytes(buf[idx:need])
            want = need
        raise TimeoutError("no reply — radio off, wrong port, or cable not seated")

    def hello(self) -> dict:
        """
        CMD_0514 -> REPLY_0515. Must be first: it sets the Timestamp that every
        later command echoes (app/uart.c:216, checked at app/uart.c:235).

        Side effect worth knowing: the firmware turns the LCD backlight off here
        (app/uart.c:225). A radio going dark IS the success signal.
        """
        self.timestamp = int(time.time()) & 0xFFFFFFFF
        payload = struct.pack("<HHI", 0x0514, 4, self.timestamp)
        self.ser.reset_input_buffer()
        self.ser.write(build_frame(payload))
        reply = parse_reply(self._read_frame())

        reply_id = struct.unpack("<H", reply[0:2])[0]
        if reply_id != 0x0515:
            raise ValueError(f"expected reply 0x0515, got 0x{reply_id:04X}")

        version = reply[4:20].split(b"\x00")[0].decode("ascii", errors="replace")
        return {
            "version": version,
            "has_custom_aes_key": bool(reply[20]),
            "in_lock_screen": bool(reply[21]),
        }

    def read_eeprom(self, offset: int, size: int) -> bytes:
        """
        CMD_051B -> REPLY_051C.

        There is NO safe-address guard on the read path: CMD_051B calls
        EEPROM_ReadBuffer() directly (app/uart.c:255), and that function has no
        bounds logic at all (driver/eeprom.c). The write guard lives elsewhere.
        So calibration at 0x1E00-0x1FFF IS readable this way.

        SILENT-FAILURE MODE, from the source (app/uart.c:250-255): if the radio
        has a custom AES key AND is locked, the firmware skips the read and
        returns a well-formed reply full of ZEROS. It does not report an error.
        That is precisely why the all-0x00 check in the runbook exists.
        """
        if size > MAX_READ_CHUNK:
            raise ValueError(f"max read is {MAX_READ_CHUNK} bytes per command")
        payload = struct.pack("<HHHBBI", 0x051B, 8, offset, size, 0, self.timestamp)
        self.ser.write(build_frame(payload))
        reply = parse_reply(self._read_frame())

        reply_id = struct.unpack("<H", reply[0:2])[0]
        if reply_id != 0x051C:
            raise ValueError(f"expected reply 0x051C, got 0x{reply_id:04X}")

        got_offset, got_size = struct.unpack("<HB", reply[4:7])
        if got_offset != offset:
            raise ValueError(f"offset mismatch: asked 0x{offset:04X}, got 0x{got_offset:04X}")
        if got_size != size:
            raise ValueError(f"size mismatch: asked {size}, got {got_size}")
        return reply[8:8 + size]

    def write_eeprom(self, offset: int, data: bytes) -> bool:
        """
        CMD_051D -> REPLY_051E (app/uart.c:260-300).

        The firmware loops `for i in range(Size / 8)` and writes 8 bytes at a
        time, so `data` MUST be a multiple of 8 bytes and `offset` should be
        8-byte aligned.

        GUARDS, both real:
        * Host-side: this refuses offset+len > 0x1E00.
        * Firmware-side: EEPROM_WriteBuffer(..., safe=true) refuses
          Address >= EEPROM_WRITE_MAX_ADDR (0x1E00). Calibration cannot be
          reached through this command even if the host lies.

        Note EEPROM_WriteBuffer skips the burn entirely when the 8 bytes already
        match (driver/eeprom.c:58), so rewriting identical data costs no wear.

        Writing into 0x0F30-0x0F3F (ENC_KEY) makes the firmware re-run
        BOARD_EEPROM_Init() (app/uart.c:290-296).
        """
        if len(data) % 8 != 0:
            raise ValueError("EEPROM writes must be a multiple of 8 bytes")
        if offset % 8 != 0:
            raise ValueError("EEPROM write offset must be 8-byte aligned")
        if offset + len(data) > CALIBRATION_START:
            raise ValueError(
                f"REFUSING write at 0x{offset:04X}+{len(data)} — would reach "
                f"calibration at 0x{CALIBRATION_START:04X}")

        payload = struct.pack("<HHHBBI", 0x051D, 8 + len(data),
                              offset, len(data), 0, self.timestamp) + data
        self.ser.write(build_frame(payload))
        reply = parse_reply(self._read_frame())
        reply_id = struct.unpack("<H", reply[0:2])[0]
        if reply_id != 0x051E:
            raise ValueError(f"expected reply 0x051E, got 0x{reply_id:04X}")
        got = struct.unpack("<H", reply[4:6])[0]
        return got == offset

    def write_eeprom_byte(self, offset: int, value: int) -> bool:
        """Read-modify-write a single byte inside its 8-byte aligned block."""
        base = offset & ~7
        block = bytearray(self.read_eeprom(base, 8))
        block[offset - base] = value
        return self.write_eeprom(base, bytes(block))

    def read_adc(self) -> dict:
        """
        CMD_0527 -> REPLY_0528 (app/uart.c:306-317).

        ExNoiseIndicator is BK4819_REG_65 & 0x007F — the exact register
        CRYPTO_RandomByte() samples bit 0 of. This is the entire reason the
        RNG entropy capture needs no firmware change.
        """
        payload = struct.pack("<HH", 0x0527, 0)
        self.ser.write(build_frame(payload))
        # REPLY_0527_t: Header(4) + Data(4) = 8 payload; frame = 2+2+8+2+2 = 16
        reply = parse_reply(self._read_frame(expect_size=16))

        reply_id = struct.unpack("<H", reply[0:2])[0]
        if reply_id != 0x0528:
            raise ValueError(f"expected reply 0x0528, got 0x{reply_id:04X}")

        rssi, ex_noise, glitch = struct.unpack("<HBB", reply[4:8])
        return {
            "rssi": rssi & 0x01FF,
            "ex_noise": ex_noise & 0x7F,   # REG_65 low 7 bits
            "rng_bit": ex_noise & 0x01,    # the bit the RNG actually uses
            "glitch": glitch,
        }

    def read_activity_log(self) -> dict:
        """
        CMD_05A0 -> REPLY_05A1 (app/uart.c). Feature #4.

        Read-only and safe to poll: the radio does NOT clear the log on read,
        so a dropped reply loses nothing. Dedupe on (freq_or_chan, ticks).

        There is no RTC on the DP32G030, so entries are stamped with uptime,
        not wall-clock. The reply carries the radio's current tick count so the
        host can render each entry as "how long ago".
        """
        payload = struct.pack("<HH", 0x05A0, 0)
        self.ser.write(build_frame(payload))
        reply = parse_reply(self._read_frame())
        reply_id = struct.unpack("<H", reply[0:2])[0]
        if reply_id != 0x05A1:
            raise ValueError(f"expected reply 0x05A1, got 0x{reply_id:04X}")

        count, head, size, _pad = struct.unpack("<BBBB", reply[4:8])
        uptime_ticks = struct.unpack("<I", reply[8:12])[0]
        entries = []
        base = 12
        for i in range(size):
            off = base + i * 8
            freq, ticks, rssi, flags = struct.unpack("<IHBB", reply[off:off + 8])
            entries.append({"freq_or_chan": freq, "ticks": ticks,
                            "rssi": rssi, "flags": flags})
        # walk newest-first from the head
        ordered = []
        for n in range(count):
            ordered.append(entries[(head - n) % size])
        return {"count": count, "head": head, "size": size,
                "uptime_ticks": uptime_ticks, "entries": ordered}

    def read_battery(self) -> dict:
        """
        CMD_0529 -> REPLY_052A (app/uart.c:319-330).

        Note the firmware's own comment: "Original doesn't actually send current!"
        Only Voltage is populated by BOARD_ADC_GetBatteryInfo(); the Current field
        is whatever was on the stack. Do not report it as a reading.
        """
        payload = struct.pack("<HH", 0x0529, 0)
        self.ser.write(build_frame(payload))
        reply = parse_reply(self._read_frame())
        reply_id = struct.unpack("<H", reply[0:2])[0]
        if reply_id != 0x052A:
            raise ValueError(f"expected reply 0x052A, got 0x{reply_id:04X}")
        voltage, _current_unused = struct.unpack("<HH", reply[4:8])
        return {"voltage_raw": voltage}

    def read_adc_batch(self, count: int) -> list:
        """
        Pipelined CMD_0527 — the throughput fix.

        Why this works, from the source: UART_DMA_Buffer is 256 bytes
        (driver/uart.c:30) and the main loop drains exactly one command per pass
        (app/app.c:1141-1144). A CMD_0527 frame is 12 bytes on the wire, so ~21
        fit in the DMA ring. Sending a batch and then reading the replies pays
        the FTDI 16ms latency timer ONCE for the whole batch instead of once per
        sample.

        Keep count well under 21 so the ring cannot wrap mid-batch. Short reads
        are tolerated: we return whatever parsed cleanly, and the caller keeps
        its own host timestamps.
        """
        if count > 16:
            raise ValueError("batch too large for the 256-byte DMA ring")

        payload = struct.pack("<HH", 0x0527, 0)
        frame = build_frame(payload)
        self.ser.write(frame * count)

        want = 16 * count
        buf = bytearray()
        deadline = time.time() + 3.0
        while len(buf) < want and time.time() < deadline:
            chunk = self.ser.read(want - len(buf))
            if chunk:
                buf += chunk
            elif buf:
                break  # radio stopped early — take what we have

        out = []
        pos = 0
        while True:
            idx = buf.find(FRAME_HEAD, pos)
            if idx < 0 or len(buf) < idx + 16:
                break
            try:
                reply = parse_reply(bytes(buf[idx:idx + 16]))
                if struct.unpack("<H", reply[0:2])[0] == 0x0528:
                    rssi, ex_noise, glitch = struct.unpack("<HBB", reply[4:8])
                    out.append({
                        "rssi": rssi & 0x01FF,
                        "ex_noise": ex_noise & 0x7F,
                        "rng_bit": ex_noise & 0x01,
                        "glitch": glitch,
                    })
            except ValueError:
                pass
            pos = idx + 16
        return out
