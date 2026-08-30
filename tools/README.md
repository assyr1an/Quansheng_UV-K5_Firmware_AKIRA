# AKIRA host tooling

Every protocol constant in these tools is traced to a file and line in this tree — see the
header of `k5_protocol.py` for the citation list. Python 3, one dependency: `pip install pyserial`.

| File | What it does |
|---|---|
| `k5_protocol.py` | The UART protocol: framing, obfuscation, CRC-16/XMODEM, hello, EEPROM read/write. Shared library for everything below |
| `k5_eeprom_dump.py` | Full 8 KB EEPROM backup with three verification conditions. **Run this before anything writes to the radio** |
| `k5_guard.py` | Safety gate: compares the radio's EEPROM against a baseline dump and **stops the session if factory RF calibration has changed** |
| `k5_flash.py` | Firmware flasher (radio in bootloader mode: hold PTT, switch on). Refuses to write at or above `0xF000`, so it cannot touch the bootloader |
| `k5_provision.py` | Generates the 256-bit master key on the host (`secrets.token_bytes`), writes key / sender ID / counter to EEPROM `0x1D00`, reads back and verifies. `--show` reports the provisioning state |
| `k5_selftest.py` | Post-flash self-check: version, battery, RSSI, and a byte-level EEPROM comparison against the pre-flash backup |
| `k5_channels.py` | Programs memory channels from a JSON plan file (`channels.example.json`), read-back verified |
| `k5_activity.py` | Dumps the scanner activity log over UART |
| `k5_flock.py` | Reads/sets the TX band policy (`F Lock`) and its sub-toggles — the menu item the radio hides unless booted holding PTT + SIDE1. Read-back verified |
| `v2-reference/` | The protocol v2 host reference (pure Python) and the known-answer vectors the firmware must reproduce byte-for-byte |
| `v2-vectors-test/` | Compiles the firmware's own v2 codec for the host and checks it against the vectors — `bash run.sh`. No radio needed |

## Safety properties

- **Nothing here can key the transmitter.** No tool sends any command that initiates TX, by
  deliberate omission.
- The EEPROM write path refuses addresses at or above `0x1E00` — factory RF calibration —
  **twice**: once in the firmware, once host-side before sending.
- The flasher refuses addresses at or above `0xF000`, so the bootloader cannot be overwritten.
- Every write the crypto depends on is read back and verified.

## Typical sequence

```sh
# 1. First contact — hello + one ADC read. Read-only, proves cable/port/protocol.
python tools/k5_eeprom_dump.py --port COM22 --info

# 2. Full 8KB backup, BEFORE flashing anything. Keep it on two drives.
python tools/k5_eeprom_dump.py --port COM22 --radio k5-A

# 3. Flash (radio in bootloader mode: hold PTT, switch on)
python tools/k5_flash.py --port COM22 --file firmware.bin --yes

# 4. Verify
python tools/k5_selftest.py --port COM22 --expect-version AKIRA
python tools/k5_guard.py --port COM22 --baseline <backup>.raw --allow 0x0E80-0x0E88

# 5. Provision the messenger key (first radio mints the key; later radios share it)
python tools/k5_provision.py --port COM22 --new-key --keyfile v2-key.json
```

`0x0E80–0x0E88` drifts benignly on every power cycle (the saved VFO channel indices) — always
`--allow` it in the guard.
