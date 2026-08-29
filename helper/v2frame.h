/* Messenger protocol v2 - frame codec.
 *
 * Spec: the workspace the protocol spec
 * Host reference: the host tools v2-reference/v2_frame.py
 * Known-answer vectors: the host tools v2-reference/vectors.json
 *
 * THIS FILE DELIBERATELY TOUCHES NO HARDWARE. It includes chacha and poly1305
 * and nothing else, so the v2 vector test  can compile it on the
 * host and check it byte-for-byte against vectors.json. That turns "is the
 * firmware crypto right?" into a matching exercise instead of a question we
 * could only answer with two radios.
 *
 * Frame (56 bytes, even so the FSK FIFO's 16-bit writes land exactly):
 *
 *    0   1  ver = 0x02
 *    1   1  type   1=MSG 2=ENC_MSG 3=ACK 4=ENC_ACK
 *    2   4  sender_id  (LE)
 *    6   4  counter    (LE)
 *   10  30  ciphertext
 *   40  16  Poly1305 tag
 *
 * The 10-byte header is the AAD, so ver/type/sender_id/counter are all
 * authenticated - otherwise an attacker could redirect an ACK by editing the
 * header. Every frame is encrypted and authenticated; the "plain" type values
 * are domain-separation labels, not an unprotected mode.
 */

#ifndef HELPER_V2FRAME_H
#define HELPER_V2FRAME_H

#include <stdbool.h>
#include <stdint.h>

enum {
	V2_VER          = 0x02,

	V2_HEADER_LEN   = 10,
	V2_PAYLOAD_LEN  = 30,
	V2_TAG_LEN      = 16,
	V2_FRAME_LEN    = V2_HEADER_LEN + V2_PAYLOAD_LEN + V2_TAG_LEN,  /* 56 */

	V2_KEY_LEN      = 32,
	V2_NONCE_LEN    = 12,

	// Characters of the on-radio key fingerprint. 6 x 5 bits = 30 bits, so two
	// different keys collide with probability ~2^-30. That is ample for "do our
	// two radios hold the same key?", which is the only question it answers -
	// and an attacker cannot choose keys here anyway, since both radios are
	// provisioned from one host keyfile.
	V2_FINGERPRINT_LEN = 6
};

/* Packet types. Values match the host reference exactly. */
enum {
	V2_TYPE_MSG     = 1,
	V2_TYPE_ENC_MSG = 2,
	V2_TYPE_ACK     = 3,
	V2_TYPE_ENC_ACK = 4
};

typedef struct {
	uint32_t sender_id;
	uint32_t counter;
	uint8_t  type;
	uint8_t  payload[V2_PAYLOAD_LEN];
} V2Message_t;

/* nonce = sender_id[4] || counter[4] || type[1] || 00 00 00
 *
 * Deterministic, never random. Nonces need UNIQUENESS, not unpredictability,
 * which is exactly why the measured weakness of the hardware RNG
 * (the RNG measurements) stops mattering here. `type` keeps a message and
 * its own ACK on separate nonces. */
void V2_BuildNonce(uint8_t nonce[V2_NONCE_LEN],
                   uint32_t sender_id, uint32_t counter, uint8_t type);

/* Build one complete 56-byte frame. payload_len <= V2_PAYLOAD_LEN; the payload
 * is zero-padded to the full 30 bytes so every frame is the same length and
 * leaks no length information. */
void V2_Encode(uint8_t frame[V2_FRAME_LEN], const uint8_t key[V2_KEY_LEN],
               uint8_t type, uint32_t sender_id, uint32_t counter,
               const uint8_t *payload, uint8_t payload_len);

/* Returns false - and writes nothing useful to *out - if the frame is the
 * wrong version or fails authentication. A false return means forged,
 * corrupted, or wrong key, and the caller must not display any part of it. */
bool V2_Decode(V2Message_t *out, const uint8_t key[V2_KEY_LEN],
               const uint8_t frame[V2_FRAME_LEN]);

/* A short, human-comparable fingerprint of the master key, so two operators can
 * confirm on-screen that their radios hold the same key with no laptop present.
 * Writes V2_FINGERPRINT_LEN characters plus a NUL.
 *
 * It does NOT weaken the key: the output is ChaCha20 keystream at a fixed
 * nonce, which is PRF output, and only 30 bits of it are published.
 *
 * The fingerprint nonce is domain-separated from every message nonce by
 * construction. A message nonce is sender_id||counter||type||000 with type in
 * 1..4, so byte 8 is never zero; the fingerprint nonce has byte 8 == 0. The two
 * keystreams therefore never overlap, and no fingerprint can ever reveal a
 * keystream byte used to encrypt a message. */
void V2_Fingerprint(char out[V2_FINGERPRINT_LEN + 1],
                    const uint8_t key[V2_KEY_LEN]);

#endif
