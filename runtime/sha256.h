#ifndef PSIV_SHA256_H
#define PSIV_SHA256_H

#include <stddef.h>
#include <stdint.h>

void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
/* first 8 bytes of the digest as 16 upper-case hex digits (BizHawk style) */
void sha256_hex16(const uint8_t *data, size_t len, char out[17]);

#endif
