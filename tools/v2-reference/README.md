# Protocol v2 — host reference

**The authority the firmware must match.** Pure Python, no dependencies, written to mirror what the
C will do so the two can be compared step by step rather than only at the output.

```sh
python v2_frame.py --selftest    # RFC vectors + cross-check + round-trip + tamper tests
python v2_frame.py --emit        # regenerate vectors.json
```

## Why this exists

Every meaningful test of the v2 crypto needs two radios. Writing the firmware against a verified
reference turns it into a **matching exercise** — the C either reproduces `vectors.json`
byte-for-byte or it does not — rather than writing both sides blind with no way to tell which is
wrong.

## It has already paid for itself

Building it caught a **catastrophic flaw in the spec's original key schedule**, before any firmware
existed. An earlier draft derived a fixed `K_mac` and used it as the Poly1305 key. Poly1305 is a
one-time authenticator: reusing its key across two messages lets an attacker solve for `r` and
forge arbitrarily. That would have replaced "no MAC" with "a MAC that can be stripped after two
messages."

The fix is to follow RFC 8439 exactly — a per-message one-time key from ChaCha20 block 0 — which
also means **v2 needs no key derivation at all**. The spec (`docs/PROTOCOL.md`) is corrected at
source.

## What is verified

| Group | Checks |
|---|---|
| RFC 8439 known-answer | ChaCha20 block §2.3.2 · Poly1305 §2.5.2 · AEAD ciphertext + tag §2.8.2 |
| Independent cross-check | byte-for-byte against `python-cryptography`'s `ChaCha20Poly1305` |
| Round-trip | 56-byte frame, even length, all header fields and payload preserved |
| **Authentication rejects** | flipped ciphertext bit · tampered `sender_id` · tampered `counter` · flipped tag bit · wrong key |
| Nonce uniqueness | counter, `sender_id` and `type` each change the nonce; 12 bytes exactly |
| Authenticated ACK | identifies the exact `(sender_id, counter)` it acknowledges |

The tamper group is the one that matters most: it is the property v1 does not have at all.

## Frame

```
 0   1  ver = 0x02
 1   1  type   1=MSG 2=ENC_MSG 3=ACK 4=ENC_ACK
 2   4  sender_id  (LE)
 6   4  counter    (LE)
10  30  ciphertext
40  16  Poly1305 tag        -> 56 bytes total, even
```

The 10-byte header is the **AAD**, so `ver`, `type`, `sender_id` and `counter` are authenticated.
Without that an attacker could redirect an ACK by editing the header.

`nonce = sender_id[4] ‖ counter[4] ‖ type[1] ‖ 00 00 00` — deterministic, never random. Nonces need
uniqueness, not unpredictability, which removes any dependence on the radio's measurably weak
hardware RNG. `type` keeps a message and its own ACK on separate nonces.

## Note for the firmware implementation

`aead_decrypt()` compares tags in **constant time**. The C must do the same — a `memcmp` that exits
early leaks tag bytes through timing.
