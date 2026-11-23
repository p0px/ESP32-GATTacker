#include "ble/payload.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  const char *invalid[] = {"0", "001", "GG", "0 ", "-1", "0x"};
  uint16_t length = 37;
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    assert(!decode_hex_payload(invalid[i], &length, 517));
    assert(length == 37);
  }
  assert(!decode_hex_payload(NULL, &length, 517));
  assert(!decode_hex_payload("00", NULL, 517));
  uint8_t *empty = decode_hex_payload("", &length, 517);
  assert(empty && length == 0);
  free(empty);

  char hex[1037];
  for (unsigned count = 0; count <= 518; count++) {
    for (unsigned i = 0; i < count; i++) {
      snprintf(hex + i * 2, 3, "%02x", i & 255);
    }
    hex[count * 2] = 0;
    length = 23;
    uint8_t *bytes = decode_hex_payload(hex, &length, 517);
    if (count == 518) {
      assert(!bytes && length == 23);
    } else {
      assert(bytes && length == count);
      for (unsigned i = 0; i < count; i++) assert(bytes[i] == (i & 255));
    }
    free(bytes);
  }
}
