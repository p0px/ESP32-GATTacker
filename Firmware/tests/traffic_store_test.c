#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble/traffic_store.h"

#define CONFIG_SPIRAM 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
static uint32_t clock_ms;
static void portENTER_CRITICAL(portMUX_TYPE *mux) { assert(!*mux); *mux = 1; }
static void portEXIT_CRITICAL(portMUX_TYPE *mux) { assert(*mux); *mux = 0; }
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static void *heap_caps_calloc(size_t count, size_t size, int caps) {
  assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return calloc(count, size);
}
static size_t strlcpy(char *destination, const char *source, size_t size) {
  size_t length = strlen(source);
  if (size) {
    size_t copy = length < size - 1 ? length : size - 1;
    memcpy(destination, source, copy);
    destination[copy] = 0;
  }
  return length;
}

#include "traffic_store_production.inc"

int main(void) {
  traffic_store_init();
  TrafficPosition position = traffic_store_position();
  assert(position.capacity == 512 && position.oldest == 1 && position.newest == 0);
  TrafficEvent event;
  assert(!traffic_store_read(0, &event));
  for (uint32_t i = 1; i <= 520; i++) {
    clock_ms = i * 10;
    traffic_store_record("READ CHAR RSP", "target -> central", "CHAR",
                         i, 3, i % 2, "2a00", "0102");
  }
  position = traffic_store_position();
  assert(position.oldest == 9 && position.newest == 520 && position.uptime_ms == 5200);
  assert(!traffic_store_read(8, &event));
  assert(traffic_store_read(9, &event));
  assert(event.cursor == 9 && event.time_ms == 90 && event.transaction == 9);
  assert(event.status == 1 && event.handle == 3 && event.has_data && !event.truncated);
  assert(!strcmp(event.operation, "READ CHAR RSP") && !strcmp(event.data, "0102"));
  assert(traffic_store_read(520, &event) && event.cursor == 520);
  char large[TRAFFIC_VALUE_MAX + 3];
  memset(large, 'a', sizeof(large));
  large[sizeof(large) - 1] = 0;
  traffic_store_record("NOTIFY", "target -> central", "", 0, 4, 0, "2a01", large);
  assert(traffic_store_read(521, &event) && event.truncated);
  assert(strlen(event.data) == TRAFFIC_VALUE_MAX);
  traffic_store_record("READ", "central -> target", "CHAR", 522, 5, 0, "2a02", NULL);
  assert(traffic_store_read(522, &event) && !event.has_data && !event.truncated);
  free(events);
}
