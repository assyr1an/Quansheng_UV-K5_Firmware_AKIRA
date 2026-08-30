# AKIRA

**Custom firmware for the Quansheng UV-K5, built for household emergency preparedness.**

Authenticated, encrypted radio-to-radio messaging that works with no network, no
infrastructure and no third party — plus the monitoring features needed to know what is on
the air around you.

> **Status: 0.9 — feature complete, not yet validated over the air.**
> Everything below is built, reviewed and CI-green, and runs on real hardware. But no message
> has yet crossed between two radios: no ACK has returned, no duplicate has been suppressed,
> no retry has fired. `1.0.0` is reserved for when the 27-test bench list passes on a pair.

## What AKIRA adds

**Messenger protocol v2** — a complete replacement for the v1 wire format.

* **RFC 8439 ChaCha20-Poly1305** on every frame, with the header as additional authenticated
  data, so the version, type, sender and counter are authenticated alongside the payload.
* **No key derivation at all.** A 256-bit master key is provisioned over USB from the host's
  CSPRNG. v1 expanded a 16-byte secret with FNV-1, which collapsed the key to at most 2⁶⁴
  values regardless of input.
* **Deterministic nonces** — `sender_id ‖ counter ‖ type`. Nonces need uniqueness, not
  unpredictability, which removes any dependence on the radio's very weak hardware RNG.
* **Replay and duplicate suppression** — a per-sender counter window. A duplicate is
  re-acknowledged but never re-displayed; an older frame is dropped silently.
* **Authenticated ACKs** that name the exact message they acknowledge, so a stale ACK cannot
  confirm a different transaction.
* **Automatic retry** by byte-identical retransmission, consuming no additional nonce.
* **A different FSK sync word from v1**, so the two protocols are invisible to each other at
  the hardware layer.

**Security and monitoring**

* **Two-gesture panic wipe** — one press clears every plaintext buffer; a confirmed second
  press destroys the master key in EEPROM as well as RAM. It works from the main screen *and*
  from inside the spectrum analyser.
* **On-radio key fingerprint** (`KeyID`) so two operators can confirm they hold the same key
  with no computer present.
* **Priority-channel scanning** with a configurable interval, **scan-hit auto-store**, a
  **bounded activity log**, and a **16-entry message ring** with paging.
* **Host tooling** — EEPROM backup and verification, firmware flashing, key provisioning,
  channel programming, a self-test, and a safety gate that refuses to let a write proceed if
  factory RF calibration has changed.

**Removed on purpose:** AirCopy (it clones the encryption key over the air, and would clone the
per-radio sender identity into nonce reuse) and the v1 `EncKey` / `MsgEnc` menus (they edited a
secret protocol v2 does not read).

## Lineage and licence

AKIRA is a fork of [kamilsss655/uv-k5-firmware-custom](https://github.com/kamilsss655/uv-k5-firmware-custom)
(the "nunu" firmware), which is itself a fork of
[Egzumer](https://github.com/egzumer/uv-k5-firmware-custom), derived in turn from
[joaquimorg](https://github.com/joaquimorg) and
[DualTachyon](https://github.com/DualTachyon)'s open re-implementation of the stock firmware.

Licensed under the Apache License 2.0. Copyright notices in inherited files belong to their
original authors and are unchanged; files original to AKIRA carry their own. **The vast
majority of this radio's behaviour — the RF driver, the UI, the spectrum analyser, the scanner
— is upstream work, and AKIRA would not exist without it.**

Forked at upstream v.20.5. Note that the mesh "NUNU Protocol" advertised by upstream landed in
v.21.0 and is **not** present in this tree — AKIRA's messenger is a flat broadcast group.

---

## Documentation

| | |
|---|---|
| [`docs/BUILDING.md`](docs/BUILDING.md) | Build, flash, provision, verify |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | The messenger v2 wire format and semantics, and the bench list gating 1.0.0 |
| [`docs/SECURITY.md`](docs/SECURITY.md) | Threat model, construction, and the limitations |
| [`tools/`](tools/) | Host tooling — backup, flash, provision, verify, and the host-side crypto tests |
| [`CHANGELOG.md`](CHANGELOG.md) | What changed, and what is still unproven |

## Hardware

Quansheng UV-K5, UV-K5(8) and UV-K6. A USB programming cable is required for flashing and for
key provisioning — the K5's is a K-type 2-pin connector, and an FTDI-based cable is
recommended.

**Back up before you flash.** Factory RF calibration at EEPROM `0x1E00-0x1FFF` is unique to your
radio and unrecoverable; CHIRP does not capture it. `docs/BUILDING.md` covers this first, and
the included `k5_guard.py` refuses to let a session continue if calibration has changed.

## Building

```sh
docker build -t akira .
docker run --rm -v "$PWD/out:/out" akira \
  /bin/bash -c "cd /app && make && cp firmware.bin /out/"
```

Full instructions, including the bootloader sequence and the three post-flash checks, are in
[`docs/BUILDING.md`](docs/BUILDING.md).

## Status

**0.9 — feature complete, not validated over the air.** Everything is built, reviewed, CI-green
and running on hardware. No message has yet crossed between two radios. See the changelog for
what that leaves unproven.

## Credits

AKIRA stands on a long chain of work, and the radio's core behaviour — the RF driver, the UI,
the spectrum analyser, the scanner — is not mine:

* [kamilsss655](https://github.com/kamilsss655) — the fork AKIRA is built from, including the
  FSK messenger this project's protocol v2 replaces
* [Egzumer](https://github.com/egzumer) — the fork that one came from
* [Joaquimorg](https://github.com/joaquimorg) and
  [DualTachyon](https://github.com/DualTachyon) — the open re-implementation everything rests on
* [OneOfEleven](https://github.com/OneOfEleven), [Mikhail](https://github.com/fagci),
  [Andrej](https://github.com/Tunas1337), [Manuel](https://github.com/manujedi),
  [@Matoz](https://github.com/spm81) and the many others credited upstream

## Licence

Apache License 2.0. Copyright notices in inherited files belong to their original authors and
are unchanged; files original to AKIRA carry their own.

> [!WARNING]
> Users are responsible for ensuring compliance with all local laws and regulations governing
> the use of this technology. This firmware comes with no warranty of any kind, and carries a
> risk of rendering a radio unusable. Use it at your own risk.
