#include "wire.h"
#include <assert.h>
#include <string.h>

static unsigned received;
static void on_frame(void *context, WireKind kind, uint16_t id, const char *text) {
  (void)context;
  assert(kind == WireCommand && id == 42);
  assert(!strcmp(text, "hello\t~}"));
  received++;
}

int main(void) {
  uint8_t frame[WIRE_FRAME_MAX];
  size_t size = wire_encode(WireCommand, 42, "hello\t~}", frame, sizeof(frame));
  assert(size > 0);
  WireParser parser = {0};
  for (size_t i = 0; i < size; i++) wire_feed(&parser, frame[i], on_frame, NULL);
  assert(received == 1);
  frame[size - 3] ^= 1;
  for (size_t i = 0; i < size; i++) wire_feed(&parser, frame[i], on_frame, NULL);
  assert(received == 1);
  size = wire_encode(WireCommand, 42, "hello\t~}", frame, sizeof(frame));
  for (size_t i = 0; i < size; i++) wire_feed(&parser, frame[i], on_frame, NULL);
  assert(received == 2);
}
