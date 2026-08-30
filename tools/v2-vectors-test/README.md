# v2 vector test — the firmware's own crypto, checked on a laptop

```sh
bash run.sh            # native gcc
bash run.sh --docker   # or inside the image that builds the firmware
```

## What it does

It compiles **the firmware's actual source** — `helper/v2frame.c`, `helper/poly1305.c`,
`external/chacha/chacha.c`, straight out of the repository root — for the host, and checks it
against `../v2-reference/vectors.json`: the 6 known-answer frames the Python reference emitted,
which were themselves cross-checked byte-for-byte against `python-cryptography`.

Not a re-implementation. Not a model of the firmware. The same C.

## Why it exists

Every meaningful over-the-air test of protocol v2 needs two radios. Without this, the firmware
crypto would sit unvalidated behind that blocker, and the first time anyone found out it was wrong
would be with two radios in hand and no way to tell which side was at fault.

This turns it into a matching exercise settled on a laptop: the C either reproduces the reference
frame byte-for-byte or it does not.

## What each case checks

| Check | Detail |
|---|---|
| Nonce construction | `V2_BuildNonce` matches `nonce_hex` |
| **Encode** | The 56-byte frame matches `frame_hex` **byte-for-byte** |
| Decode | The reference frame authenticates and round-trips to the original payload, sender, counter and type |
| **Tamper** | All **56** single-byte flips are rejected — every header byte, every ciphertext byte, every tag byte |
| Wrong key | A frame under a one-bit-different key is rejected, not silently turned into garbage |

336 tamper rejections across the 6 cases. That group is the point: it is the property v1 did not
have at all, and the reason `MSG_HandleReceive()` can be an authentication gate rather than a
display path.

Compiled with `-Wall -Wextra -Werror -O2`. Re-run after touching `helper/v2frame.c`,
`helper/poly1305.c` or `external/chacha/chacha.c`.

## Regenerating

`gen_vectors.py` re-expresses `../v2-reference/vectors.json` as `vectors.inc` so the test binary
needs no JSON parser. `run.sh` runs it every time, so the JSON stays the single source of truth.
If the reference changes, re-run `python ../v2-reference/v2_frame.py --emit` first.

## What this does NOT prove

- That a 56-byte frame survives the FSK FIFO and the air. **Needs two radios.**
- That the counter reservation, EEPROM provisioning, or the sync-word change behave on hardware.
  Those live in `app/messenger.c`, which this test cannot compile — it touches hardware by design.
- Timing. The constant-time tag compare is by construction, not measured.

## Related

- `../v2-reference/` — the Python host reference and `vectors.json`
- `../../docs/PROTOCOL.md` — the spec these vectors implement
