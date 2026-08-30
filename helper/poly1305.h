/* poly1305-donna, 32-bit reference implementation.
 * Public domain (Andrew Moon), https://github.com/floodyberry/poly1305-donna
 *
 * Verified against the RFC 8439 section 2.5.2 test vector and measured at
 * 1,572 bytes of .text on this target before being adopted.
 *
 * Poly1305 is a ONE-TIME authenticator. Its key must never be reused across
 * two messages: two tags under the same (r, s) let an attacker solve for r and
 * forge arbitrarily. v2 therefore derives the key per message from ChaCha20
 * block 0 (RFC 8439 section 2.6) - see helper/v2frame.c.
 */

#ifndef HELPER_POLY1305_H
#define HELPER_POLY1305_H

#include <stdint.h>

typedef struct {
	uint32_t r[5], h[5], pad[4];
	uint32_t leftover;
	unsigned char buffer[16];
	unsigned char final;
} poly1305_ctx;

void poly1305_init(poly1305_ctx *st, const unsigned char key[32]);
void poly1305_update(poly1305_ctx *st, const unsigned char *m, uint32_t bytes);
void poly1305_finish(poly1305_ctx *st, unsigned char mac[16]);

#endif
