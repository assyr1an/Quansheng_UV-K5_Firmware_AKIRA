/* Messenger protocol v2 - frame codec. See v2frame.h for the frame layout and
 * for why this file must stay free of hardware dependencies.
 *
 * The construction is RFC 8439 ChaCha20-Poly1305 exactly:
 *
 *   otk        = ChaCha20_block(key, counter=0, nonce)[0..31]     (per message)
 *   ciphertext = ChaCha20(key, counter=1, nonce) XOR payload
 *   tag        = Poly1305(otk, aad || pad16 || ct || pad16 || len(aad) || len(ct))
 *
 * The ChaCha20 key IS the provisioned 256-bit master key. There is no key
 * derivation, and that is the point: the spec originally derived a FIXED K_mac
 * and used it as the Poly1305 key, which would have allowed arbitrary forgery
 * after two messages. RFC 8439's per-message one-time key removes both the
 * flaw and the need for a KDF. See the protocol spec section 3.
 */

#include <string.h>

#include "v2frame.h"
#include "poly1305.h"
#include "external/chacha/chacha.h"

/* ChaCha20 with an explicit 32-bit block counter. The bundled chacha is already
 * RFC 8439-shaped - input[12] is a 32-bit counter and input[13..15] the 96-bit
 * nonce (external/chacha/chacha.c, chacha_ivsetup) - so no patching is needed. */
static void V2_ChaCha(const uint8_t key[V2_KEY_LEN], uint32_t block_counter,
                      const uint8_t nonce[V2_NONCE_LEN],
                      const uint8_t *in, uint8_t *out, uint32_t len)
{
	struct chacha_ctx ctx;
	uint8_t ctr[4];

	ctr[0] = (uint8_t)(block_counter >> 0);
	ctr[1] = (uint8_t)(block_counter >> 8);
	ctr[2] = (uint8_t)(block_counter >> 16);
	ctr[3] = (uint8_t)(block_counter >> 24);

	chacha_keysetup(&ctx, key, 256);
	chacha_ivsetup(&ctx, nonce, ctr);
	chacha_encrypt_bytes(&ctx, in, out, len);

	memset(&ctx, 0, sizeof(ctx));
}

static void V2_PutLE32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 0);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t V2_GetLE32(const uint8_t *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void V2_BuildNonce(uint8_t nonce[V2_NONCE_LEN],
                   uint32_t sender_id, uint32_t counter, uint8_t type)
{
	V2_PutLE32(&nonce[0], sender_id);
	V2_PutLE32(&nonce[4], counter);
	nonce[8]  = type;
	nonce[9]  = 0;
	nonce[10] = 0;
	nonce[11] = 0;
}

/* RFC 8439 section 2.8.1. The AAD and ciphertext are each padded to a 16-byte
 * boundary, then the two lengths follow as 64-bit little-endian values.
 * Fed incrementally so no contiguous MAC buffer is ever assembled - both pads
 * and the length block are fixed-size here, but the general form is kept so a
 * future variable-length payload cannot silently break the padding. */
static void V2_Tag(uint8_t tag[V2_TAG_LEN], const uint8_t otk[32],
                   const uint8_t *aad, uint32_t aad_len,
                   const uint8_t *ct, uint32_t ct_len)
{
	static const uint8_t zeros[16] = { 0 };
	poly1305_ctx st;
	uint8_t lengths[16];

	poly1305_init(&st, otk);

	poly1305_update(&st, aad, aad_len);
	poly1305_update(&st, zeros, (16u - (aad_len & 15u)) & 15u);
	poly1305_update(&st, ct, ct_len);
	poly1305_update(&st, zeros, (16u - (ct_len & 15u)) & 15u);

	memset(lengths, 0, sizeof(lengths));
	V2_PutLE32(&lengths[0], aad_len);   /* high 32 bits stay zero: both */
	V2_PutLE32(&lengths[8], ct_len);    /* lengths are far below 4 GB   */
	poly1305_update(&st, lengths, sizeof(lengths));

	poly1305_finish(&st, tag);
	memset(&st, 0, sizeof(st));
}

void V2_Encode(uint8_t frame[V2_FRAME_LEN], const uint8_t key[V2_KEY_LEN],
               uint8_t type, uint32_t sender_id, uint32_t counter,
               const uint8_t *payload, uint8_t payload_len)
{
	uint8_t nonce[V2_NONCE_LEN];
	uint8_t otk[32];

	if (payload_len > V2_PAYLOAD_LEN)
		payload_len = V2_PAYLOAD_LEN;

	frame[0] = V2_VER;
	frame[1] = type;
	V2_PutLE32(&frame[2], sender_id);
	V2_PutLE32(&frame[6], counter);

	/* Zero-pad the payload to the full 30 bytes before encrypting. */
	memset(&frame[V2_HEADER_LEN], 0, V2_PAYLOAD_LEN);
	if (payload_len > 0)
		memcpy(&frame[V2_HEADER_LEN], payload, payload_len);

	V2_BuildNonce(nonce, sender_id, counter, type);

	/* RFC 8439 section 2.6: the one-time Poly1305 key is the first 32 bytes of
	 * ChaCha20 block 0. Encrypting zeros yields the raw keystream. */
	memset(otk, 0, sizeof(otk));
	V2_ChaCha(key, 0, nonce, otk, otk, sizeof(otk));
	V2_ChaCha(key, 1, nonce, &frame[V2_HEADER_LEN], &frame[V2_HEADER_LEN],
	          V2_PAYLOAD_LEN);

	V2_Tag(&frame[V2_HEADER_LEN + V2_PAYLOAD_LEN], otk,
	       frame, V2_HEADER_LEN, &frame[V2_HEADER_LEN], V2_PAYLOAD_LEN);

	memset(otk, 0, sizeof(otk));
	memset(nonce, 0, sizeof(nonce));
}

void V2_Fingerprint(char out[V2_FINGERPRINT_LEN + 1], const uint8_t key[V2_KEY_LEN])
{
	// Crockford base32: no I, L, O or U, so nothing is misread aloud or
	// mistaken for a digit when two people compare six characters over a radio.
	static const char alphabet[32] = {
		'0','1','2','3','4','5','6','7','8','9',
		'A','B','C','D','E','F','G','H','J','K',
		'M','N','P','Q','R','S','T','V','W','X','Y','Z'
	};
	// Byte 8 is 0, which no message nonce can ever be - see v2frame.h.
	static const uint8_t fp_nonce[V2_NONCE_LEN] = {
		'K','E','Y','I','D', 0, 0, 0, 0, 0, 0, 0
	};
	uint8_t ks[V2_FINGERPRINT_LEN];
	uint8_t i;

	memset(ks, 0, sizeof(ks));
	V2_ChaCha(key, 0, fp_nonce, ks, ks, sizeof(ks));

	for (i = 0; i < V2_FINGERPRINT_LEN; i++)
		out[i] = alphabet[ks[i] & 31u];
	out[V2_FINGERPRINT_LEN] = 0;

	memset(ks, 0, sizeof(ks));
}

bool V2_Decode(V2Message_t *out, const uint8_t key[V2_KEY_LEN],
               const uint8_t frame[V2_FRAME_LEN])
{
	uint8_t nonce[V2_NONCE_LEN];
	uint8_t otk[32];
	uint8_t expect[V2_TAG_LEN];
	uint8_t diff = 0;
	uint8_t i;

	if (frame[0] != V2_VER)
		return false;

	out->type      = frame[1];
	out->sender_id = V2_GetLE32(&frame[2]);
	out->counter   = V2_GetLE32(&frame[6]);

	V2_BuildNonce(nonce, out->sender_id, out->counter, out->type);

	memset(otk, 0, sizeof(otk));
	V2_ChaCha(key, 0, nonce, otk, otk, sizeof(otk));

	V2_Tag(expect, otk, frame, V2_HEADER_LEN,
	       &frame[V2_HEADER_LEN], V2_PAYLOAD_LEN);

	/* Constant time. A memcmp() that exits on the first difference leaks the
	 * tag one byte at a time through timing, which is exactly the oracle an
	 * attacker needs to forge without knowing the key. */
	for (i = 0; i < V2_TAG_LEN; i++)
		diff |= expect[i] ^ frame[V2_HEADER_LEN + V2_PAYLOAD_LEN + i];

	if (diff != 0) {
		memset(otk, 0, sizeof(otk));
		memset(nonce, 0, sizeof(nonce));
		return false;
	}

	/* Only now, with the tag verified, is any plaintext produced. */
	V2_ChaCha(key, 1, nonce, &frame[V2_HEADER_LEN], out->payload,
	          V2_PAYLOAD_LEN);

	memset(otk, 0, sizeof(otk));
	memset(nonce, 0, sizeof(nonce));
	return true;
}
