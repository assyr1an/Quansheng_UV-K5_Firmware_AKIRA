# Security model

What AKIRA defends against, what it does not, and why. Read the limitations section — it is the
honest half.

## Reporting a vulnerability

For an **exploitable weakness** — something an attacker could use against people already
running this firmware — please use GitHub's **private vulnerability reporting** on this
repository (Security tab → "Report a vulnerability") rather than a public issue, and allow
time for a fix before disclosure.

**Design findings are different**: gaps in the threat model, weaknesses in the protocol as
specified, disagreements with a stated security claim. Those are welcome as ordinary public
issues — adversarial review is exactly what a pre-1.0 protocol needs, and this document
exists to be argued with.

---

## The threat model, in order

This firmware was built for household emergency use, and the ranking follows from that rather
than from a generic checklist.

| # | Threat | Addressed? |
|---|---|---|
| 1 | **Physical capture of a radio** | Partly — panic wipe destroys messages and, on a confirmed second press, the key |
| 2 | **Forgery** — someone injecting messages that appear to come from your group | **Yes.** Poly1305 over header and ciphertext |
| 3 | **Modification** — flipping bits in a message in flight | **Yes.** Same tag |
| 4 | **Replay** — recording a real message and re-sending it later | **Yes.** Per-sender counter window |
| 5 | **False delivery confirmation** — a spoofed ACK | **Yes.** ACKs are authenticated and name their message |
| 6 | **Interception** — reading message content off the air | **Yes**, while the key holds |
| 7 | **Traffic analysis** — who is talking, how much | **No, by design** |
| 8 | **Jamming / denial of service** | **No** |

Interception sits below forgery on purpose. An adversary who can inject convincing messages into
an emergency comms channel does more damage than one who can merely read it.

---

## Construction

RFC 8439 ChaCha20-Poly1305, used exactly as specified.

```
key        = K_master                                  256 bits, provisioned over USB
nonce      = sender_id ‖ counter ‖ type ‖ 00 00 00     96 bits, deterministic
otk        = ChaCha20_block(key, counter=0, nonce)     per message, RFC 8439 §2.6
ciphertext = ChaCha20(key, counter=1, nonce) XOR payload
tag        = Poly1305(otk, header ‖ pad ‖ ct ‖ pad ‖ len(header) ‖ len(ct))
```

**The header is the AAD.** Version, type, sender ID and counter are authenticated along with the
payload — otherwise an attacker could redirect an ACK by editing the header.

**There is no key derivation.** The provisioned key *is* the ChaCha20 key. Poly1305 gets a fresh
one-time key per message from block 0, which is what RFC 8439 does and what makes a KDF
unnecessary.

> A fixed Poly1305 key is a total break, not a weakening. Poly1305 is a one-time authenticator:
> two tags under the same `(r, s)` let an attacker solve for `r` and forge arbitrarily. An
> earlier draft of this design specified exactly that. It was caught by building a host
> reference before writing any firmware.

### Nonces are deterministic, and that is the point

Nonces need **uniqueness**, not unpredictability. Deriving them from `sender_id ‖ counter`
removes any dependence on the radio's hardware RNG — which was measured at roughly 56 usable
bits in 96, with 52.6% bit bias, failing NIST SP 800-90B repetition count.

Uniqueness rests on the counter alone, so the counter is the thing protected hardest:

- Reserved in blocks of 64, written to EEPROM, and **read back and verified**. The driver's
  write is fire-and-forget, so an unverified reservation is the one silent path to nonce reuse.
- A failed verification **stops transmission** until the identity is reloaded. Refusing to send
  is loud and recoverable; reusing a nonce is silent and permanent.
- A reboot jumps to the next block. Counters may skip; they can never repeat.
- Exhaustion refuses to transmit rather than wrapping.

---

## Keys

Generated on the **host** with `secrets.token_bytes` and written over USB. Never generated on
the radio.

Stored at EEPROM `0x1D00`, the one region a CHIRP upload cannot reach — the driver's
`PROG_SIZE` is `0x1d00`, so it reads the whole EEPROM but writes only below that.

**One key per group.** Every radio holds the same key and a different sender ID. Two radios
sharing a sender ID would reuse nonces, so the provisioning tool refuses to issue a duplicate.

`KeyID` shows a six-character fingerprint so two operators can confirm they match with no
computer present. It publishes 30 bits of ChaCha20 keystream at a nonce **domain-separated by
construction**: message nonces carry a type of 1–4 at byte 8, the fingerprint nonce carries 0
there, so the two keystreams can never overlap.

---

## Panic wipe

Two bindable actions:

| Action | Effect |
|---|---|
| `PANIC WIPE` | Clears every plaintext buffer — message ring, compose line, recall buffer, staged payload, decoded frame, activity log, replay window, pending retry |
| `WIPE +KEY` | The above on the first press; **destroys the master key, sender ID and counter** on a confirmed second press within 3 seconds |

The asymmetry is deliberate. A panic press must never be spent waiting for confirmation, so the
messages go immediately. The key needs a second press because wiping it strands the radio until
it is re-provisioned from the keyfile over a cable — which in the field may be neither to hand
nor safe to produce.

Details that matter:

- The identity is overwritten with `0xFF`, not `0x00`, so the result is indistinguishable from a
  radio never provisioned. Zeros in a field of `0xFF` announce that something used to be there.
- EEPROM is cleared **before** RAM: if the battery is pulled halfway, the key is already gone.
- Every block is **read back**. If the wipe cannot be proved, the screen says `KEY LEFT!` rather
  than claiming success. A wipe that lies is worse than one that fails.
- It works on the main screen **and** inside the spectrum analyser — which has its own key
  handler and previously ignored it entirely.

---

## Limitations

**No forward secrecy.** A captured radio yields the master key, and therefore every message
anyone recorded off the air. The panic wipe helps only if someone is present to use it. Rotating
the key after any loss is the only real answer, and it means re-provisioning every radio.

**Sender IDs are in the clear.** The receiver needs them to reconstruct the nonce before it can
decrypt, so they cannot be hidden. Anyone listening can count messages and attribute them to a
radio.

**No protection against jamming or flooding.**

**The replay window is 4 entries and RAM-only.** It resets on reboot, costing at most one
re-displayed message. With more than four active senders an entry can be evicted, which resets
that sender's window.

**The radio is deaf while the spectrum screen is open.** It runs no timeslices, so messages,
UART and timers all stop. Entry is refused while a message is in flight, and the status line
says `MSG RX OFF`, but the underlying limitation stands.

**The UART link is obfuscated, not encrypted.** Key provisioning crosses it in the clear. That
is acceptable here because the threat model puts physical capture first, not someone sitting on
the programming cable — but it is a real assumption, not an oversight.

**The messenger has not been validated over the air.** As of 0.9, voice operation is verified
against a second transceiver, and the construction is verified against known-answer vectors on
the host — but no v2 message has crossed between two AKIRA radios, so the FSK message link
itself is unproven.
