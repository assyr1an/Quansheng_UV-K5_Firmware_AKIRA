/* Host-side known-answer test for the FIRMWARE's v2 frame codec.
 *
 * It compiles helper/v2frame.c, helper/poly1305.c and external/chacha/chacha.c
 * straight out of the firmware tree - the same source the radio runs - and
 * checks them against tools/v2-reference/vectors.json, which the
 * Python host reference emitted and which was itself cross-checked byte-for-byte
 * against python-cryptography.
 *
 * The point of this file: every meaningful over-the-air test of v2 needs two
 * radios and we have one. This makes the firmware crypto a matching exercise
 * that can be settled on a laptop, and it fails loudly the moment the C drifts
 * from the reference by a single byte.
 *
 * Build and run:  bash run.sh
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "helper/v2frame.h"

/* Vectors are inlined rather than parsed from JSON so the test has no
 * dependencies at all. Regenerate with gen_vectors.py if vectors.json changes. */
#include "vectors.inc"

static int hex2bin(const char *hex, unsigned char *out, size_t max)
{
	size_t n = strlen(hex) / 2, i;
	if (n > max) return -1;
	for (i = 0; i < n; i++) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
		out[i] = (unsigned char)v;
	}
	return (int)n;
}

static void dump(const char *label, const unsigned char *b, size_t n)
{
	size_t i;
	printf("    %-10s", label);
	for (i = 0; i < n; i++) printf("%02x", b[i]);
	printf("\n");
}

int main(void)
{
	unsigned char key[V2_KEY_LEN];
	int fails = 0, i;
	const int ncases = (int)(sizeof(CASES) / sizeof(CASES[0]));

	if (hex2bin(MASTER_KEY_HEX, key, sizeof(key)) != V2_KEY_LEN) {
		printf("bad master key in vectors\n");
		return 2;
	}

	/* Key fingerprint - the on-radio "do our two radios hold the same key?"
	 * check. Verified here rather than trusted, because a mistake would be
	 * invisible on the radio: two DIFFERENT keys showing the same six
	 * characters is exactly the failure it must not have. */
	{
		char fp[V2_FINGERPRINT_LEN + 1];
		unsigned char other[V2_KEY_LEN];
		char fp2[V2_FINGERPRINT_LEN + 1];

		V2_Fingerprint(fp, key);
		if (strcmp(fp, EXPECT_FINGERPRINT) != 0) {
			printf("[FAIL] fingerprint: want %s, got %s\n", EXPECT_FINGERPRINT, fp);
			fails++;
		} else {
			printf("[ ok ] fingerprint   %s\n", fp);
		}

		/* A one-bit key change must change the fingerprint. */
		memcpy(other, key, sizeof(other));
		other[0] ^= 0x01;
		V2_Fingerprint(fp2, other);
		if (strcmp(fp, fp2) == 0) {
			printf("[FAIL] fingerprint unchanged for a 1-bit key difference\n");
			fails++;
		}
	}

	printf("v2 frame codec vs %s\n", VECTORS_SPEC);
	printf("%d cases, master key %s\n\n", ncases, MASTER_KEY_HEX);

	for (i = 0; i < ncases; i++) {
		const struct vcase *c = &CASES[i];
		unsigned char want_frame[V2_FRAME_LEN];
		unsigned char want_nonce[V2_NONCE_LEN];
		unsigned char payload[V2_PAYLOAD_LEN];
		unsigned char got_frame[V2_FRAME_LEN];
		unsigned char got_nonce[V2_NONCE_LEN];
		V2Message_t   msg;
		int plen, ok = 1;

		if (hex2bin(c->frame_hex, want_frame, sizeof(want_frame)) != V2_FRAME_LEN) {
			printf("[case %s] malformed expected frame\n", c->name);
			fails++; continue;
		}
		hex2bin(c->nonce_hex, want_nonce, sizeof(want_nonce));
		memset(payload, 0, sizeof(payload));
		plen = hex2bin(c->payload_hex, payload, sizeof(payload));

		/* 1. nonce construction */
		V2_BuildNonce(got_nonce, c->sender_id, c->counter, c->type);
		if (memcmp(got_nonce, want_nonce, V2_NONCE_LEN) != 0) {
			printf("[FAIL] %s: nonce mismatch\n", c->name);
			dump("want", want_nonce, V2_NONCE_LEN);
			dump("got",  got_nonce,  V2_NONCE_LEN);
			ok = 0;
		}

		/* 2. encode reproduces the reference frame byte-for-byte */
		V2_Encode(got_frame, key, c->type, c->sender_id, c->counter,
		          payload, (unsigned char)plen);
		if (memcmp(got_frame, want_frame, V2_FRAME_LEN) != 0) {
			printf("[FAIL] %s: frame mismatch\n", c->name);
			dump("want", want_frame, V2_FRAME_LEN);
			dump("got",  got_frame,  V2_FRAME_LEN);
			ok = 0;
		}

		/* 3. decode of the reference frame authenticates and round-trips */
		memset(&msg, 0, sizeof(msg));
		if (!V2_Decode(&msg, key, want_frame)) {
			printf("[FAIL] %s: decode rejected a valid reference frame\n", c->name);
			ok = 0;
		} else if (msg.type != c->type || msg.sender_id != c->sender_id ||
		           msg.counter != c->counter ||
		           memcmp(msg.payload, payload, V2_PAYLOAD_LEN) != 0) {
			printf("[FAIL] %s: decode round-trip mismatch\n", c->name);
			ok = 0;
		}

		/* 4. tamper: every single-byte edit must be rejected. This is the
		 *    property v1 does not have at all. */
		{
			int b, tampered_accepted = 0;
			for (b = 0; b < V2_FRAME_LEN; b++) {
				unsigned char t[V2_FRAME_LEN];
				V2Message_t   junk;
				memcpy(t, want_frame, sizeof(t));
				t[b] ^= 0x01;
				if (V2_Decode(&junk, key, t)) {
					printf("[FAIL] %s: accepted a frame with byte %d flipped\n",
					       c->name, b);
					tampered_accepted = 1;
				}
			}
			if (tampered_accepted) ok = 0;
		}

		/* 5. wrong key must be rejected, not silently produce garbage */
		{
			unsigned char wrong[V2_KEY_LEN];
			V2Message_t   junk;
			memcpy(wrong, key, sizeof(wrong));
			wrong[31] ^= 0x80;
			if (V2_Decode(&junk, wrong, want_frame)) {
				printf("[FAIL] %s: accepted a frame under the wrong key\n", c->name);
				ok = 0;
			}
		}

		printf("%s %-12s  type=%d sender=%08x counter=%u\n",
		       ok ? "[ ok ]" : "[FAIL]", c->name,
		       c->type, c->sender_id, c->counter);
		if (!ok) fails++;
	}

	printf("\n%d/%d cases passed", ncases - fails, ncases);
	printf(" (encode + decode + %d single-byte tamper rejections + wrong-key rejection each)\n",
	       V2_FRAME_LEN);
	return fails ? 1 : 0;
}
