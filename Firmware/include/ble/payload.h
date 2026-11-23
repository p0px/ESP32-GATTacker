#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// On failure, leave the caller's length and original payload unchanged.
static inline uint8_t *decode_hex_payload(const char *hex, uint16_t *length, size_t limit) {
  if (!hex || !length)
    return NULL;
  size_t n = strlen(hex);
  if (n % 2 || n / 2 > limit || n / 2 > UINT16_MAX)
    return NULL;
  for (size_t i = 0; i < n; ++i) {
    if (hex_nibble(hex[i]) < 0)
      return NULL;
  }
  uint8_t *bytes = (uint8_t *)malloc(n ? n / 2 : 1);
  if (!bytes)
    return NULL;
  for (size_t i = 0; i < n / 2; ++i) {
    bytes[i] = (hex_nibble(hex[i * 2]) << 4) | hex_nibble(hex[i * 2 + 1]);
  }
  *length = n / 2;
  return bytes;
}
