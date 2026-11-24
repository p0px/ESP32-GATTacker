#include "ble/traffic_store.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

static TrafficEvent *events;
static uint32_t capacity;
static uint32_t newest;
static portMUX_TYPE traffic_mux = portMUX_INITIALIZER_UNLOCKED;

void traffic_store_init(void) {
  if (events) return;
#ifdef CONFIG_SPIRAM
  capacity = 512;
  events = heap_caps_calloc(capacity, sizeof(*events), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!events) {
    capacity = 32;
    events = heap_caps_calloc(capacity, sizeof(*events), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!events) capacity = 0;
}

void traffic_store_record(const char *operation, const char *direction, const char *kind,
                          uint32_t transaction, uint16_t handle, uint32_t status,
                          const char *uuid, const char *data) {
  if (!events) return;
  bool truncated = data && strlen(data) > TRAFFIC_VALUE_MAX;
  portENTER_CRITICAL(&traffic_mux);
  uint32_t cursor = ++newest;
  TrafficEvent *event = &events[cursor % capacity];
  event->cursor = cursor;
  event->time_ms = esp_timer_get_time() / 1000;
  event->transaction = transaction;
  event->status = status;
  event->handle = handle;
  event->has_data = data != NULL;
  event->truncated = truncated;
  strlcpy(event->operation, operation ? operation : "", sizeof(event->operation));
  strlcpy(event->direction, direction ? direction : "", sizeof(event->direction));
  strlcpy(event->kind, kind ? kind : "", sizeof(event->kind));
  strlcpy(event->uuid, uuid ? uuid : "", sizeof(event->uuid));
  strlcpy(event->data, data ? data : "", sizeof(event->data));
  portEXIT_CRITICAL(&traffic_mux);
}

TrafficPosition traffic_store_position(void) {
  TrafficPosition position;
  portENTER_CRITICAL(&traffic_mux);
  position.newest = newest;
  position.capacity = capacity;
  position.oldest = capacity && newest > capacity ? newest - capacity + 1 : 1;
  portEXIT_CRITICAL(&traffic_mux);
  position.uptime_ms = esp_timer_get_time() / 1000;
  return position;
}

bool traffic_store_read(uint32_t cursor, TrafficEvent *event) {
  if (!events || !event || !cursor) return false;
  portENTER_CRITICAL(&traffic_mux);
  const TrafficEvent *source = &events[cursor % capacity];
  const bool present = source->cursor == cursor;
  if (present) *event = *source;
  portEXIT_CRITICAL(&traffic_mux);
  return present;
}
