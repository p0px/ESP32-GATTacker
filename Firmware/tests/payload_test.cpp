#include "ble/payload.h"
#include <assert.h>
#include <string>

int main() {
  uint16_t length = 37;
  for (const char *bad : {"0", "001", "GG", "0 ", "-1", "0x"}) {
    assert(!decode_hex_payload(bad, &length, 517));
    assert(length == 37);
  }
  assert(!decode_hex_payload(nullptr, &length, 517));
  assert(!decode_hex_payload("00", nullptr, 517));
  uint8_t *empty = decode_hex_payload("", &length, 517);
  assert(empty && length == 0);
  free(empty);

  // Round-trip every byte, repeatedly, including the maximum response size.
  for (int round = 0; round < 10000; ++round) {
    std::string hex;
    const unsigned n = round % 518;
    for (unsigned i = 0; i < n; ++i) {
      char byte[3];
      snprintf(byte, sizeof(byte), "%02x", i & 255);
      hex += byte;
    }
    uint8_t *bytes = decode_hex_payload(hex.c_str(), &length, 517);
    assert(bytes && length == n);
    for (unsigned i = 0; i < n; ++i) assert(bytes[i] == (i & 255));
    free(bytes);
  }
  length = 23;
  assert(!decode_hex_payload(std::string(1036, 'A').c_str(), &length, 517));
  assert(length == 23);
  assert(!decode_hex_payload(std::string(131072, 'A').c_str(), &length, 100000));
  assert(length == 23);
}
