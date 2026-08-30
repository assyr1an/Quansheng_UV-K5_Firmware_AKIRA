# Messenger protocol v2

The wire format and semantics of AKIRA's radio-to-radio messenger. This is the specification the
firmware implements; `tools/v2-reference/` is the executable form of it, and
`tools/v2-vectors-test/` proves the firmware's codec matches it byte-for-byte.

**Status: implemented, host-verified, not yet validated over the air.** The bench list at the end
is what stands between 0.9 and 1.0.0.

---

## Why v1 had to be replaced

Three verified failures in the inherited messenger, each traced to source before anything was
designed:

1. **The key derivation collapsed the key to 64 bits.** A 16-byte secret was expanded to 32 bytes
   with FNV-1 — an invertible multiply with public salts, so any one output chunk reverses to the
   shared pre-salt state. The "256-bit key" spanned at most 2⁶⁴ values regardless of input.
2. **No MAC, and the hardware CRC explicitly disabled.** Raw ChaCha20 is malleable: forgery,
   bit-flipping, spoofed delivery notifications, and undetectable wrong-key output.
3. **Nonces came from a measurably weak hardware RNG** — roughly 56 usable bits in 96, 52.6% bit
   bias, failing NIST SP 800-90B repetition count.

The wire format had no version byte, no tag, and no sequence number, so v2 is a replacement, not a
patch. There is deliberately no compatibility path — see Versioning.

## Frame

56 bytes, even (the FSK FIFO is written as 16-bit words):

```
offset  size  field
  0      1    ver        = 0x02
  1      1    type       1=MSG  2=ENC_MSG  3=ACK  4=ENC_ACK
  2      4    sender_id  (LE)
  6      4    counter    (LE)
 10     30    ciphertext
 40     16    Poly1305 tag
```

The 10-byte header is the **AAD**: `ver`, `type`, `sender_id` and `counter` are authenticated
alongside the payload. Without that, an attacker could redirect an ACK by editing the header.

## Construction

RFC 8439 ChaCha20-Poly1305, used exactly as specified:

```
key        = K_master                                    256 bits, provisioned over USB
nonce      = sender_id[4] ‖ counter[4] ‖ type[1] ‖ 00 00 00
otk        = ChaCha20_block(key, counter=0, nonce)[0..31]     per message, RFC 8439 §2.6
ciphertext = ChaCha20(key, counter=1, nonce) XOR payload
tag        = Poly1305(otk, AAD ‖ pad ‖ ct ‖ pad ‖ len(AAD) ‖ len(ct))
```

**There is no key derivation.** The provisioned key *is* the ChaCha20 key; Poly1305 gets a fresh
one-time key per message from block 0. An earlier draft of this spec derived a *fixed* `K_mac` —
which is a total break, not a weakening: Poly1305 is a one-time authenticator, and two tags under
the same `(r, s)` let an attacker solve for `r` and forge arbitrarily. It was caught by building
the host reference before writing any firmware.

Tag comparison is constant-time on both the reference and the firmware.

## Nonces and the counter

Nonces need **uniqueness, not unpredictability** — that is what removes the dependence on the
radio's RNG. Only `sender_id` and `counter` go on the wire; the receiver reconstructs the rest.
`type` domain-separates a message from its own ACK.

Uniqueness rests entirely on the counter, so the counter is protected hardest:

- **Reserved in blocks of 64**: the counter lives in RAM, and `counter + 64` is written to EEPROM
  only on crossing a block boundary. On boot the radio jumps to the next block, so a reboot skips
  at most 64 values — harmless, since only uniqueness matters — and an EEPROM cell endures one
  write per 64 messages.
- Every reservation write is **read back and verified**. The EEPROM driver's write is
  fire-and-forget; an unverified reservation would be the one silent path to nonce reuse. A failed
  verification stops transmission until the identity is reloaded.
- The counter never goes backwards, and exhaustion refuses to transmit rather than wrapping.
- An unprovisioned radio — blank key, `sender_id` of 0 or `0xFFFFFFFF`, counter `0xFFFFFFFF` —
  refuses to transmit and beeps.

## Keys

Generated on the **host** (`secrets.token_bytes`), written over USB by `tools/k5_provision.py`,
never generated on the radio.

Stored at EEPROM `0x1D00` — the one usable region a CHIRP upload cannot reach, since the CHIRP
driver writes only below `0x1D00`:

| Offset | Size | Field |
|---|---:|---|
| `0x1D00` | 32 | `K_master` |
| `0x1D20` | 4 | `sender_id` — per-radio, minted once at provisioning |
| `0x1D24` | 4 | `counter` — persisted in blocks of 64 |
| `0x1D28`–`0x1DFF` | 216 | reserved |

**One key per group, one `sender_id` per radio.** Two radios sharing a `sender_id` would reuse
nonces, so the provisioning tool refuses to issue a duplicate. This is also one of the two reasons
AirCopy was removed: it clones `0x0000–0x1DFF`, which would clone both the key and the sender
identity.

`KeyID` in the menu shows a 6-character fingerprint — 30 bits of ChaCha20 keystream at a nonce
carrying `type = 0`, which no message nonce can carry — so two operators can confirm they hold the
same key with no computer present.

## Versioning

v2 uses a **different 4-byte FSK sync word** from v1, so the two protocols never even raise an
interrupt for each other: no version-confusion handling, no downgrade path, no garbage frames
reaching the parser, at a cost of zero payload bytes. The `ver` byte exists for evolution *within*
v2.

The BK4819's hardware CRC is left **disabled** for now: whether `REG_5C` bit 6 is transparent to
the declared frame length is not settled by the source, and getting it wrong breaks FSK receive
undiagnosably. Poly1305 already rejects every corrupted frame with certainty; the CRC would buy
only earlier rejection. It is a one-line bench experiment (test 9 below).

## ACK, replay, and retry

**Authenticated ACK.** An ACK is a full v2 frame with its own `counter`, whose payload names the
`(sender_id, counter)` it acknowledges — under the tag. The sender requires a match against its
outstanding transaction; a stale or foreign ACK confirms nothing.

**Replay window.** Each receiver keeps 4 RAM-only entries of `sender_id → highest_counter_seen`:

- `counter > highest` — accept, update, display. Only a frame whose tag has verified may move the
  window: **authentication first, replay second**, or a forged frame could lock out the real
  sender.
- `counter == highest` — duplicate: re-ACK, do **not** re-display. This is what makes retry safe.
- `counter < highest` — replay: drop silently.

The window resets on reboot, costing at most one re-displayed message.

**Retry retransmits the byte-identical frame** — same counter, nonce, ciphertext and tag — so it
consumes no nonce and the far end deduplicates it. 4-second timeout, 3 transmissions total, `MsgRty`
menu toggle, `!` on the log line when attempts are exhausted. ACKs are never retried (that way lies
an ACK storm). Re-encrypting under a fresh counter would be wrong here: it would display twice.

## Airtime

A 56-byte frame carries 996 ms of payload airtime at FSK-450 — before preamble and sync — against
what was a 1000 ms TX timeout upstream (whose comment claimed 310 ms). The timeout is 2000 ms in
AKIRA.

---

## Bench validation — the 27 tests between 0.9 and 1.0.0

Everything above is verified on a laptop or not at all. The list is ordered so the two results that
could invalidate the whole design fail first. Radios A and B share a key and differ in `sender_id`.

| # | Test | Pass condition |
|:-:|---|---|
| 1 | **Provision both radios** from one keyfile — `k5_provision.py --new-key` on A, plain on B | Both read back MATCH; `sender_id`s differ |
| 2 | **A 56-byte frame survives the FIFO** — send A→B on FSK-450, the slowest case | Message displays on B |
| 3 | Repeat at FSK-700 and AFSK-1200 | Same |
| 4 | **Authenticated ACK returns** | A shows `+` on the sent line |
| 5 | **Wrong key is rejected silently** — reprovision B with a different key, send from A | B displays nothing |
| 6 | **v1 and v2 radios are deaf to each other** — flash one radio to a pre-v2 build, send both ways | Neither sees the other's traffic, no garbage frames |
| 7 | **Counter survives a power cycle** — send 3, power cycle, send 3 more, reading `0x1D24` each time | Counter never repeats, never goes backwards; skips ≤64 are expected |
| 8 | **Unprovisioned radio refuses to transmit** — blank `0x1D00` on B and try to send | Double beep, nothing transmitted |
| 9 | **Hardware CRC experiment** — `REG_5C` `0x5625` → `0x5665` on both radios, repeat test 2 | Either outcome settles the question; revert if it fails |
| 10 | **Airtime** — time a send at FSK-450 against the 2000 ms timeout | Completes well inside |
| 11 | **WIPE+KEY, first press only** — bind to a side key, press once, let the window lapse | Messages gone; prompt clears; `k5_provision.py --show` still reports the key |
| 12 | **WIPE+KEY, both presses** — press twice inside the 3 s window | `KEY GONE`; `--show` reports UNPROVISIONED at all three fields; a send attempt refuses |
| 13 | **Re-provision after a wipe** | New `sender_id` minted (the tool refuses to reissue one), counter restarts, messaging works |
| 14 | **Duplicate suppression** — have A retransmit an identical frame (pull B's antenna during the first ACK) | B displays the message once, re-ACKs with a new counter; A marks `+` |
| 15 | **Replay rejection** — capture a frame, re-send it after newer traffic has passed | B displays nothing, no ACK |
| 16 | **ACK matching** — with A awaiting an ACK, have B send an ACK naming a different counter | A does not mark `+` |
| 17 | **Replay window resets on reboot** — power-cycle B, resend the last message | B re-displays it once (RAM-only, by design — costs exactly one message) |
| 18 | **Retry fires on a lost ACK** — B's antenna off, restore after ~2 s | A retransmits at ~4 s; B displays once; A ends `+` |
| 19 | **Retry gives up** — B switched off | Exactly 3 transmissions, then `!` |
| 20 | **No ACK storm** — both radios with ACK and retry on, send one message | Traffic stops after the ACK. If the radios transmit at each other indefinitely, power both off |
| 21 | **`MsgRty` off** — disable on A, repeat test 19 | One transmission, no retry, no `!` |
| 22 | **Bind `WIPE+KEY`**, long-press once on the main screen | Wipe prompt for ~3 s, then clears; the key survives |
| 23 | **The auto-repeat trap** — in spectrum, long-press and **keep holding** the bound key ~5 s | The prompt appears once and the key survives. Spectrum's key dispatch auto-repeats every ~60 ms; without the latch this would be a one-press key wipe |
| 24 | **Panic wipe reaches spectrum** — in spectrum, press the bound key twice within 3 s | `KEY GONE`; re-provision afterwards |
| 25 | **Spectrum blackout is visible** — with message RX on, open spectrum | `MSG RX OFF` in the status line |
| 26 | **Spectrum entry refused mid-transaction** — with a retry pending, try to open spectrum | Double beep, screen stays on main |
| 27 | **Re-run `k5_selftest.py --baseline`** after all of it | Calibration at `0x1E00–0x1FFF` byte-identical. If not, stop immediately |

**Test 7 is the one to be pedantic about.** Every other failure is visible and recoverable. A
repeated counter produces two frames under one nonce, which leaks the XOR of the plaintexts and
destroys the Poly1305 one-time key for both — silently, with the messenger apparently working.

## Out of scope

Forward secrecy, traffic analysis and denial of service — see
[`SECURITY.md`](SECURITY.md), which is the authority on the threat model and limitations.
