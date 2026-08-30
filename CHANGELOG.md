# Changelog

All notable changes to AKIRA. Dates are release dates, not commit dates.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions are bare numbers — the `v0.x` and `v.x` tag series in this repository are
inherited from upstream and are *their* release history, not AKIRA's.

---

## [0.9] — 2026-08-30

First AKIRA release. Feature complete; voice operation verified against a second
transceiver; **the encrypted messenger is not yet validated over the air.**

> No v2 message has yet crossed between two AKIRA radios. No ACK has returned, no duplicate
> has been suppressed, no retry has fired. The 27-test bench list in `docs/PROTOCOL.md` is
> entirely unrun, and every test in it needs a second UV-K5 running this firmware with the
> same key. `1.0.0` is reserved for when that list passes.

### Added — messenger protocol v2

A complete replacement for the v1 wire format. 56-byte frame:

```
 0   1  ver = 0x02
 1   1  type   1=MSG 2=ENC_MSG 3=ACK 4=ENC_ACK
 2   4  sender_id  (LE)
 6   4  counter    (LE)
10  30  ciphertext
40  16  Poly1305 tag
```

- **RFC 8439 ChaCha20-Poly1305** on every frame, with the 10-byte header as additional
  authenticated data — so `ver`, `type`, `sender_id` and `counter` are authenticated
  alongside the payload. Without this an attacker could redirect an ACK by editing the header.
- **Per-message one-time Poly1305 key** from ChaCha20 block 0, ciphertext from block 1 onward.
- **UART key provisioning** — a 256-bit master key generated on the host and written to
  EEPROM `0x1D00`, the one region a CHIRP upload cannot reach.
- **Deterministic nonces** — `sender_id ‖ counter ‖ type ‖ 000`. Never transmitted; the
  receiver reconstructs them. That is where the 4 bytes for the tag came from.
- **Counter reservation in blocks of 64**, written to EEPROM and **read back and verified**.
  A failed burn refuses further transmission rather than risking nonce reuse.
- **Replay window** — 4 entries of `sender_id → highest counter seen`, RAM only. New frames
  are displayed, duplicates are re-acknowledged but never re-displayed, older frames are
  dropped silently.
- **Authenticated ACKs** naming the exact `(sender_id, counter)` they acknowledge.
- **Auto-retry** by byte-identical retransmission — same counter, nonce, ciphertext and tag —
  so a retry consumes no nonce and the far end deduplicates it. 4-second timeout,
  3 transmissions total, `MsgRty` menu toggle.
- **A different FSK sync word** from v1, so the two protocols cannot hear each other at the
  hardware layer. No version-confusion handling, no downgrade path.

### Added — security

- **Two-gesture panic wipe.** `PANIC WIPE` clears every plaintext buffer. `WIPE +KEY` does
  that on the first press, then destroys the master key, sender ID and counter — in EEPROM as
  well as RAM — on a confirmed second press within 3 seconds. Both are bindable to any side
  key, and both work from the main screen **and** from inside the spectrum analyser.
- **EEPROM writes the crypto depends on are read back and verified.** The driver's write is
  fire-and-forget, so a failed burn is otherwise silent.
- **`KeyID`** — a six-character fingerprint of the master key, so two operators can confirm
  they hold the same key with no computer present. Derived from ChaCha20 keystream at a nonce
  domain-separated from every message nonce by construction.

### Added — monitoring

- **Priority-channel scanning** on a time interval, with `PriCh1` / `PriCh2` / `PriInt`.
- **Scan-hit auto-store** to channels 150–199 only, capped and deduplicated.
- **Activity log** — 24 entries, uptime-stamped, readable over UART.
- **Message ring** — 16 entries with paging.
- **Spectrum blackout is now visible and survivable.** The panic wipe routes inside the
  modal loop, entry is refused while a message is in flight, and `MSG RX OFF` shows in the
  status line.

### Added — identity

- Boot logo, power-on tone, and the `AKIRA` name throughout.

### Removed

- **AirCopy.** It clones EEPROM `0x0000–0x1DFF`, which includes the encryption key — and
  under deterministic nonces it would clone the per-radio sender ID into guaranteed nonce
  reuse. Correctness, not a flash saving.
- **The v1 FNV-1 key derivation.** It expanded a 16-byte secret to 32 bytes with an
  invertible hash and public salts, collapsing the key to at most 2⁶⁴ values regardless of
  input. v2 provisions a full 256-bit key and derives nothing.
- **The `EncKey` and `MsgEnc` menus.** `EncKey` edited a secret v2 never reads — it would
  have let an operator "change the key", watch the display update, and keep transmitting
  under the old one. `MsgEnc` offered to disable authentication.
- **The `ENABLE_ENCRYPTION` build flag.** Removed rather than set to `0`, which would read as
  "encryption off" in a firmware where every frame is encrypted unconditionally.
- Four cosmetic build flags, reclaimed for the above.

### Fixed

- **Seven unbounded string operations on wire data.** `%s` conversions traversed buffers filled
  from the FSK FIFO with no guaranteed NUL.
- **The delivery marker landed on the wrong message.** It marked "the last line" rather than
  the message actually sent, so anything arriving between send and ACK stole the mark. It also
  accepted *any* ACK, including one for someone else's traffic.
- **A one-press key wipe.** Spectrum's key handler auto-repeats roughly every 60 ms while a key
  is held; the first version of the in-spectrum panic routing fired on every repeat, and the
  second firing destroys the key. A single long press would have skipped the confirmation.
- **An all-zero key read as provisioned.** The check rejected an erased key (`0xFF`) but not a
  zeroed one, so a failed wipe or blanked EEPROM would have left the radio transmitting under
  a key of nothing.
- **Priority scanning spent half the scan budget on two channels** — a commented-out case fell
  through, giving `PRI1, PRI2, normal, normal`. Now interval-driven.
- **The TX timeout was 1000 ms**, and its comment claimed 310 ms. Raised to 2000 ms; a 56-byte
  frame needs 996 ms of payload airtime at FSK-450 before preamble and sync.

### Known limitations

- **The radio is deaf while the spectrum screen is open.** It runs no timeslices, so messenger
  RX, UART and timers all stop. Mitigated, not fixed — the leading candidate for the next
  release.
- **No forward secrecy.** A captured radio yields the master key and therefore every message
  recorded off the air. The panic wipe addresses this only if someone is there to use it.
- **Sender IDs are in the clear**, by design — the receiver needs them to reconstruct the
  nonce before it can decrypt. Traffic can be counted and attributed.
- **The hardware CRC is left disabled.** Whether `REG_5C` bit 6 is transparent to the declared
  frame length is not settled by the source, and getting it wrong breaks FSK receive in a way
  indistinguishable from any other RX failure. Poly1305 already rejects corrupted frames.

[0.9]: https://github.com/assyr1an/Quansheng_UV-K5_Firmware_AKIRA/releases/tag/0.9
