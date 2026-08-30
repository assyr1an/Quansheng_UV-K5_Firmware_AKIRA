# Building and flashing AKIRA

Everything here has been run. Where a step can fail silently, the check that catches it is
given alongside.

---

## Build

The toolchain is pinned in a container, so the build is reproducible and needs nothing
installed beyond Docker.

```sh
docker build -t akira .
docker run --rm akira /bin/bash -c "cd /app && make && arm-none-eabi-size firmware"
```

To get the binary out, mount an output directory:

```sh
mkdir -p out
docker run --rm -v "$PWD/out:/out" akira /bin/bash -c \
  "cd /app && make && cp firmware.bin firmware.packed.bin /out/"
```

- `firmware.bin` — raw image. **This is what the flasher takes.**
- `firmware.packed.bin` — obfuscated and CRC'd, for the stock Quansheng flasher.

### The version string comes from git, inside the container

```makefile
VERSION_STRING := $(shell git describe --tags --exact-match)   # a tag, if HEAD is on one
VERSION_STRING := $(shell git rev-parse --short HEAD)          # otherwise the hash
```

**Commit before building**, or the radio will report a stale hash. This is also what makes the
reported version trustworthy: it is derived from the tree, not typed in.

The name and version are packed into a 16-byte header as `*AKIRA <version>`, so the version
has **9 characters** to work with. `0.9` and `1.0.0` fit; `1.0.0-rc1` would be truncated.

### Flash budget

The DP32G030 has **61,440 bytes** for the application; the bootloader lives above it at
`0xF000` and is never written. `make` prints the size — `text + data` must stay under the
limit. At 0.9 the build uses 60,672, leaving 768 bytes.

**Measure locally before pushing.** A CI run is a slow way to discover that something did not
fit.

---

## Flash

### 1. Back up first — this is not optional

Factory RF calibration lives at EEPROM `0x1E00–0x1FFF`. It is **unique to your radio and
unrecoverable**: CHIRP does not capture it, and no replacement exists.

```sh
python k5_eeprom_dump.py --port COM22 --radio k5-A
```

Keep the dump on **two** drives. Everything below is written on the assumption you have one.

### 2. Enter the bootloader

**Hold PTT alone and switch the radio on.** The flashlight comes on solid white and the screen
stays dark.

There is no software route. `ENABLE_OVERLAY` is `0`, so the UART reboot command resets into the
application, not the bootloader.

Confirm without writing anything:

```sh
python k5_flash.py --port COM22 --handshake-only
```

### 3. Write

```sh
python k5_flash.py --port COM22 --file out/firmware.bin --yes
```

Roughly 235 blocks. The flasher refuses to write at or above `0xF000`, so a mistake cannot
brick the bootloader.

Then **power-cycle normally, no buttons held.**

### 4. Verify — three checks, in this order

```sh
# 1. it came back, and it is the build you meant
python k5_selftest.py --port COM22 --expect-version AKIRA

# 2. nothing else in EEPROM moved
python k5_guard.py --port COM22 --baseline <pre-flash>.raw --allow 0x0E80-0x0E88

# 3. the key survived
python k5_provision.py --port COM22 --show
```

Gate 1 of `k5_guard.py` is the one that matters: **calibration byte-identical**. If it fails,
stop.

Flashing does not touch EEPROM at all — a verified flash shows the entire 8 KB unchanged, same
SHA-256. `0x0E80–0x0E88` is the exception and is benign: the VFO channel indices the radio saves
on power-off.

---

## Provision

A radio refuses to transmit messages until it holds a key, a sender ID and a counter. That is
deliberate — the alternative is falling back to something forgeable.

```sh
# first radio only, once ever
python k5_provision.py --port COM22 --new-key --keyfile v2-key.json

# every radio after, same keyfile, new sender ID minted automatically
python k5_provision.py --port COM22 --keyfile v2-key.json
```

The key is generated on the host with `secrets.token_bytes`, never on the radio — its
noise-derived RNG measures roughly 56 usable bits in 96 and fails NIST SP 800-90B repetition
count.

**The keyfile is the only copy.** Lose it before a second radio is keyed and you re-mint at no
cost; lose it after and both radios must be re-provisioned together.

Check `KeyID` in the menu matches the fingerprint the tool printed. If it does not, the key on
the radio is not the one in the file.

---

## Verifying the crypto without hardware

```sh
bash tools/v2-vectors-test/run.sh
```

Compiles the firmware's **own** `v2frame.c`, `poly1305.c` and `chacha.c` for the host and checks
them against known-answer vectors: 6 frames byte-for-byte, 336 single-byte tamper rejections,
and wrong-key rejection.

Re-run it after touching any of those three files. It proves the construction is correct; it
proves nothing about the air interface.
