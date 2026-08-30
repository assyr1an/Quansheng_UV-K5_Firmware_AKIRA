"""
Protocol v2 reference implementation — the authority the firmware must match.

Pure Python, no dependencies, deliberately written to mirror what the C will do
so the two can be compared step by step rather than only at the output.

Everything here follows RFC 8439 (ChaCha20-Poly1305) exactly. See the note in
key_schedule() for why the spec's original K_enc/K_mac split had to change.

    python v2_frame.py --selftest      # RFC vectors + cross-check + round-trip
    python v2_frame.py --emit          # write vectors.json for the firmware tests
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------- constants
VER = 0x02

TYPE_MSG = 1
TYPE_ENC_MSG = 2
TYPE_ACK = 3
TYPE_ENC_ACK = 4

HEADER_LEN = 10        # ver(1) type(1) sender_id(4) counter(4)
PAYLOAD_LEN = 30       # Profile A keeps the full v1 payload
TAG_LEN = 16           # Poly1305
FRAME_LEN = HEADER_LEN + PAYLOAD_LEN + TAG_LEN   # 56, even -> FSK FIFO writes 16-bit words


# ------------------------------------------------------------------ ChaCha20
def _rotl32(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def _quarter(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = _rotl32(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = _rotl32(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = _rotl32(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = _rotl32(s[b] ^ s[c], 7)


def chacha20_block(key: bytes, counter: int, nonce: bytes) -> bytes:
    """RFC 8439 section 2.3. 32-byte key, 12-byte nonce, 32-bit counter."""
    assert len(key) == 32 and len(nonce) == 12
    const = b"expand 32-byte k"
    state = list(struct.unpack("<4I", const)) \
        + list(struct.unpack("<8I", key)) \
        + [counter & 0xFFFFFFFF] \
        + list(struct.unpack("<3I", nonce))
    work = state[:]
    for _ in range(10):                      # 20 rounds = 10 double rounds
        _quarter(work, 0, 4, 8, 12); _quarter(work, 1, 5, 9, 13)
        _quarter(work, 2, 6, 10, 14); _quarter(work, 3, 7, 11, 15)
        _quarter(work, 0, 5, 10, 15); _quarter(work, 1, 6, 11, 12)
        _quarter(work, 2, 7, 8, 13); _quarter(work, 3, 4, 9, 14)
    out = [(work[i] + state[i]) & 0xFFFFFFFF for i in range(16)]
    return struct.pack("<16I", *out)


def chacha20_xor(key: bytes, counter: int, nonce: bytes, data: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = chacha20_block(key, counter + i // 64, nonce)
        blk = data[i:i + 64]
        out += bytes(a ^ b for a, b in zip(blk, ks))
    return bytes(out)


# ------------------------------------------------------------------ Poly1305
P1305 = (1 << 130) - 5


def poly1305_mac(msg: bytes, key: bytes) -> bytes:
    """RFC 8439 section 2.5. `key` is a ONE-TIME 32-byte key."""
    assert len(key) == 32
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    acc = 0
    for i in range(0, len(msg), 16):
        blk = msg[i:i + 16]
        n = int.from_bytes(blk + b"\x01", "little")   # append the 1 bit
        acc = ((acc + n) * r) % P1305
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def poly1305_key_gen(key: bytes, nonce: bytes) -> bytes:
    """RFC 8439 section 2.6 — the per-message one-time key."""
    return chacha20_block(key, 0, nonce)[:32]


def _pad16(b: bytes) -> bytes:
    return b"\x00" * ((16 - len(b) % 16) % 16)


def aead_encrypt(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes):
    otk = poly1305_key_gen(key, nonce)
    ct = chacha20_xor(key, 1, nonce, plaintext)       # keystream starts at block 1
    mac_data = aad + _pad16(aad) + ct + _pad16(ct) \
        + struct.pack("<Q", len(aad)) + struct.pack("<Q", len(ct))
    return ct, poly1305_mac(mac_data, otk)


def aead_decrypt(key: bytes, nonce: bytes, ciphertext: bytes, aad: bytes, tag: bytes):
    otk = poly1305_key_gen(key, nonce)
    mac_data = aad + _pad16(aad) + ciphertext + _pad16(ciphertext) \
        + struct.pack("<Q", len(aad)) + struct.pack("<Q", len(ciphertext))
    expect = poly1305_mac(mac_data, otk)
    # constant-time compare; the firmware must do the same
    diff = 0
    for a, b in zip(expect, tag):
        diff |= a ^ b
    if diff != 0 or len(tag) != len(expect):
        return None
    return chacha20_xor(key, 1, nonce, ciphertext)


# ------------------------------------------------------------ protocol layer
def key_schedule(k_master: bytes) -> bytes:
    """
    The ChaCha20 key IS the provisioned master key. There is nothing to derive.

    ------------------------------------------------------------------------
    SPEC CORRECTION 2026-08-27. The spec key schedule originally said:

        K_enc = ChaCha20(K_master, nonce=0, counter=0)[0..31]
        K_mac = ChaCha20(K_master, nonce=0, counter=1)[0..31]

    Using a FIXED K_mac as the Poly1305 key would be a catastrophic break.
    Poly1305 is a one-time authenticator in the Wegman-Carter sense: its key
    must never be reused across messages. Two messages authenticated under the
    same (r, s) let an attacker solve for r and then forge arbitrary messages.
    That is not a weakening, it is total loss of authentication - the exact
    failure v2 exists to fix.

    RFC 8439 already solves this: the Poly1305 key is generated PER MESSAGE
    from (key, nonce) via ChaCha20 block 0, and the ciphertext uses blocks 1+.
    Since our nonce is unique per message by construction (deterministic
    sender_id||counter), that gives a fresh one-time key every time.

    So v2 needs no key derivation at all - one fewer moving part than the spec
    described, and correct rather than broken.
    ------------------------------------------------------------------------
    """
    if len(k_master) != 32:
        raise ValueError("master key must be 32 bytes")
    return k_master


FINGERPRINT_LEN = 6
# Crockford base32 - no I, L, O or U, so nothing is misread aloud or mistaken
# for a digit when two people compare six characters.
_CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
# Byte 8 is 0. A message nonce is sender_id||counter||type||000 with type in
# 1..4, so its byte 8 is NEVER 0 - the two keystreams can never overlap, and a
# published fingerprint can never reveal a byte used to encrypt a message.
FP_NONCE = b"KEYID" + bytes(7)


def fingerprint(k_master: bytes) -> str:
    """Short human-comparable fingerprint of the master key.

    Publishes 30 bits of ChaCha20 keystream at a fixed, domain-separated nonce.
    That is PRF output, so it does not weaken the key; it answers only "do our
    two radios hold the same key?", which is the question two operators need to
    settle in the field with no laptop.
    """
    ks = chacha20_block(k_master, 0, FP_NONCE)[:FINGERPRINT_LEN]
    return "".join(_CROCKFORD[b & 31] for b in ks)


def build_nonce(sender_id: int, counter: int, ptype: int) -> bytes:
    """
    nonce = sender_id[4] || counter[4] || type[1] || 00 00 00

    Deterministic, never random. Nonces need UNIQUENESS, not unpredictability -
    which is why the measured weakness of the hardware RNG
    stops mattering. `type` separates a message from its own ACK so the two can
    never collide on the same (sender_id, counter).
    """
    return struct.pack("<IIB", sender_id, counter, ptype) + b"\x00\x00\x00"


def build_header(ptype: int, sender_id: int, counter: int) -> bytes:
    return struct.pack("<BBII", VER, ptype, sender_id, counter)


def encode(k_master: bytes, ptype: int, sender_id: int, counter: int,
           payload: bytes) -> bytes:
    """Build one complete 56-byte v2 frame."""
    if len(payload) > PAYLOAD_LEN:
        raise ValueError(f"payload is {len(payload)} bytes, max {PAYLOAD_LEN}")
    payload = payload.ljust(PAYLOAD_LEN, b"\x00")
    key = key_schedule(k_master)
    header = build_header(ptype, sender_id, counter)
    nonce = build_nonce(sender_id, counter, ptype)
    # The header is the AAD, so ver/type/sender_id/counter are all authenticated.
    # Without this an attacker could redirect an ACK by editing the header.
    ct, tag = aead_encrypt(key, nonce, payload, header)
    frame = header + ct + tag
    assert len(frame) == FRAME_LEN
    return frame


def decode(k_master: bytes, frame: bytes):
    """Returns a dict, or None if the frame is malformed or fails authentication."""
    if len(frame) != FRAME_LEN:
        return None
    header = frame[:HEADER_LEN]
    ct = frame[HEADER_LEN:HEADER_LEN + PAYLOAD_LEN]
    tag = frame[HEADER_LEN + PAYLOAD_LEN:]
    ver, ptype, sender_id, counter = struct.unpack("<BBII", header)
    if ver != VER:
        return None
    key = key_schedule(k_master)
    nonce = build_nonce(sender_id, counter, ptype)
    pt = aead_decrypt(key, nonce, ct, header, tag)
    if pt is None:
        return None                       # forged, corrupted, or wrong key
    return {"ver": ver, "type": ptype, "sender_id": sender_id,
            "counter": counter, "payload": pt}


def build_ack(k_master: bytes, ack_sender_id: int, ack_counter: int,
              msg_sender_id: int, msg_counter: int) -> bytes:
    """
    An authenticated ACK. The payload carries the (sender_id, counter) of the
    message being acknowledged, so a stale ACK cannot acknowledge a different
    transaction - the v1 failure the review flagged.
    """
    body = struct.pack("<II", msg_sender_id, msg_counter)
    return encode(k_master, TYPE_ENC_ACK, ack_sender_id, ack_counter, body)


def parse_ack(decoded: dict):
    if decoded is None or decoded["type"] not in (TYPE_ACK, TYPE_ENC_ACK):
        return None
    sid, ctr = struct.unpack("<II", decoded["payload"][:8])
    return {"acks_sender_id": sid, "acks_counter": ctr}


# ------------------------------------------------------------------ self-test
def selftest() -> int:
    ok = True

    def check(name, cond, extra=""):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        if extra and not cond:
            print(f"         {extra}")
        ok = ok and cond

    print("1. RFC 8439 KNOWN-ANSWER VECTORS")
    # section 2.3.2 — ChaCha20 block
    key = bytes(range(32))
    nonce = bytes.fromhex("000000090000004a00000000")
    blk = chacha20_block(key, 1, nonce)
    check("ChaCha20 block (RFC 8439 2.3.2)",
          blk.hex().startswith("10f1e7e4d13b5915500fdd1fa32071c4"), blk[:16].hex())

    # section 2.5.2 — Poly1305
    pkey = bytes.fromhex("85d6be7857556d337f4452fe42d506a8"
                         "0103808afb0db2fd4abff6af4149f51b")
    tag = poly1305_mac(b"Cryptographic Forum Research Group", pkey)
    check("Poly1305 (RFC 8439 2.5.2)",
          tag.hex() == "a8061dc1305136c6c22b8baf0c0127a9", tag.hex())

    # section 2.8.2 — full AEAD
    aead_key = bytes.fromhex("808182838485868788898a8b8c8d8e8f"
                             "909192939495969798999a9b9c9d9e9f")
    aead_nonce = bytes.fromhex("070000004041424344454647")
    aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
    pt = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."
    ct, t = aead_encrypt(aead_key, aead_nonce, pt, aad)
    check("AEAD ciphertext (RFC 8439 2.8.2)",
          ct.hex().startswith("d31a8d34648e60db7b86afbc53ef7ec2"), ct[:16].hex())
    check("AEAD tag (RFC 8439 2.8.2)",
          t.hex() == "1ae10b594f09e26a7e902ecbd0600691", t.hex())

    print()
    print("2. CROSS-CHECK AGAINST AN INDEPENDENT IMPLEMENTATION")
    try:
        from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
        ref = ChaCha20Poly1305(aead_key).encrypt(aead_nonce, pt, aad)
        check("matches python-cryptography byte-for-byte", ref == ct + t)
    except ImportError:
        print("  [SKIP] python-cryptography not installed")

    print()
    print("3. PROTOCOL ROUND-TRIP")
    km = bytes(range(0x20, 0x40))
    f = encode(km, TYPE_ENC_MSG, 0xA1B2C3D4, 42, b"hello from K5-A")
    check(f"frame is {FRAME_LEN} bytes", len(f) == FRAME_LEN, str(len(f)))
    check("frame length is even (FSK FIFO writes 16-bit words)", len(f) % 2 == 0)
    d = decode(km, f)
    check("round-trips", d is not None)
    if d:
        check("sender_id preserved", d["sender_id"] == 0xA1B2C3D4)
        check("counter preserved", d["counter"] == 42)
        check("payload preserved", d["payload"].rstrip(b"\x00") == b"hello from K5-A")

    print()
    print("4. AUTHENTICATION ACTUALLY REJECTS")
    bad = bytearray(f); bad[HEADER_LEN] ^= 0x01           # flip a ciphertext bit
    check("rejects a flipped ciphertext bit", decode(km, bytes(bad)) is None)
    bad = bytearray(f); bad[2] ^= 0x01                    # flip a sender_id bit
    check("rejects a tampered header (sender_id)", decode(km, bytes(bad)) is None)
    bad = bytearray(f); bad[6] ^= 0x01                    # flip a counter bit
    check("rejects a tampered header (counter)", decode(km, bytes(bad)) is None)
    bad = bytearray(f); bad[-1] ^= 0x01                   # flip a tag bit
    check("rejects a flipped tag bit", decode(km, bytes(bad)) is None)
    wrong = bytes(32)
    check("rejects the wrong key", decode(wrong, f) is None)

    print()
    print("5. NONCE UNIQUENESS BY CONSTRUCTION")
    n1 = build_nonce(1, 1, TYPE_ENC_MSG)
    n2 = build_nonce(1, 2, TYPE_ENC_MSG)
    n3 = build_nonce(2, 1, TYPE_ENC_MSG)
    n4 = build_nonce(1, 1, TYPE_ENC_ACK)
    check("counter changes the nonce", n1 != n2)
    check("sender_id changes the nonce", n1 != n3)
    check("type separates a message from its own ACK", n1 != n4)
    check("nonce is 12 bytes (chacha_ivsetup consumes 12)", len(n1) == 12)

    print()
    print("6. AUTHENTICATED ACK")
    a = build_ack(km, 0x11112222, 7, 0xA1B2C3D4, 42)
    pa = parse_ack(decode(km, a))
    check("ACK identifies the exact message it acknowledges",
          pa == {"acks_sender_id": 0xA1B2C3D4, "acks_counter": 42}, str(pa))

    print()
    print("=" * 62)
    print("  ALL CHECKS PASSED" if ok else "  FAILURES PRESENT")
    print("=" * 62)
    return 0 if ok else 1


def emit(path):
    """Fixed vectors the firmware implementation must reproduce byte-for-byte."""
    km = bytes(range(0x20, 0x40))
    cases = []
    for name, ptype, sid, ctr, pl in [
        ("empty",        TYPE_ENC_MSG, 0x00000001, 0,          b""),
        ("short",        TYPE_ENC_MSG, 0xA1B2C3D4, 42,         b"hello from K5-A"),
        ("full30",       TYPE_ENC_MSG, 0xDEADBEEF, 1,          b"A" * 30),
        ("counter_max",  TYPE_ENC_MSG, 0x12345678, 0xFFFFFFFF, b"rollover"),
        ("plain_msg",    TYPE_MSG,     0x00ABCDEF, 99,         b"unencrypted type"),
        ("ack",          TYPE_ENC_ACK, 0x11112222, 7,          struct.pack("<II", 0xA1B2C3D4, 42)),
    ]:
        f = encode(km, ptype, sid, ctr, pl)
        cases.append({
            "name": name, "type": ptype, "sender_id": sid, "counter": ctr,
            "payload_hex": pl.hex(),
            "nonce_hex": build_nonce(sid, ctr, ptype).hex(),
            "header_hex": build_header(ptype, sid, ctr).hex(),
            "frame_hex": f.hex(),
        })
    doc = {
        "spec": "Protocol v2 Profile A (ChaCha20-Poly1305, RFC 8439)",
        "generated_by": "v2_frame.py",
        "master_key_hex": km.hex(),
        "fingerprint": fingerprint(km),
        "frame_len": FRAME_LEN,
        "header_len": HEADER_LEN,
        "payload_len": PAYLOAD_LEN,
        "tag_len": TAG_LEN,
        "note": "The ChaCha20 key IS the master key. Poly1305 uses a PER-MESSAGE "
                "one-time key from RFC 8439 2.6 - never a fixed K_mac.",
        "cases": cases,
    }
    with open(path, "w") as fh:
        json.dump(doc, fh, indent=2)
    print(f"wrote {len(cases)} vectors -> {path}")
    for c in cases:
        print(f"  {c['name']:<12} {c['frame_hex'][:32]}...")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors.json"))
    a = ap.parse_args()
    if a.emit:
        return emit(a.out)
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
