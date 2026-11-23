#include "capture_ring.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

CaptureRing *capture_ring_create(void) {
#ifndef CONFIG_SPIRAM
  return NULL;
#else
  CaptureRing *ring = heap_caps_calloc(1, sizeof(*ring),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ring) return NULL;
  ring->packets = heap_caps_malloc(sizeof(CapturePacket) * CAPTURE_RING_CAPACITY,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ring->packets) {
    heap_caps_free(ring);
    return NULL;
  }
  portMUX_TYPE unlocked = portMUX_INITIALIZER_UNLOCKED;
  ring->lock = unlocked;
  return ring;
#endif
}

void capture_ring_destroy(CaptureRing *ring) {
  if (!ring) return;
  heap_caps_free(ring->packets);
  heap_caps_free(ring);
}

const char *capture_ring_begin(CaptureRing *ring, uint32_t token, uint32_t seconds,
                               bool web) {
  if (!ring || !token) return "HCI capture unavailable";
  portENTER_CRITICAL(&ring->lock);
  if (ring->active || ring->ending || ring->begin) {
    const char *error = ring->web ? "Web recording is active; stop it in the web UI"
                                  : "Flipper recording is active; save it on the Flipper";
    portEXIT_CRITICAL(&ring->lock);
    return error;
  }
  memset(&ring->stats, 0, sizeof(ring->stats));
  ring->stats.token = token;
  ring->head = ring->count = 0;
  ring->begin = true;
  ring->finished = ring->health_sent = false;
  ring->origin = ring->lease = esp_timer_get_time();
  ring->epoch = (uint64_t)seconds * 1000000;
  ring->web = web;
  ring->active = true;
  portEXIT_CRITICAL(&ring->lock);
  return NULL;
}

bool capture_ring_stop(CaptureRing *ring, uint32_t token, bool web) {
  if (!ring) return false;
  portENTER_CRITICAL(&ring->lock);
  bool matched = ring->stats.token == token && ring->web == web;
  if (matched) {
    ring->active = false;
    if (!ring->finished) ring->ending = true;
  }
  portEXIT_CRITICAL(&ring->lock);
  return matched;
}

bool capture_ring_keepalive(CaptureRing *ring, uint32_t token) {
  if (!ring) return false;
  portENTER_CRITICAL(&ring->lock);
  bool matched = ring->active && !ring->web && ring->stats.token == token;
  if (matched) ring->lease = esp_timer_get_time();
  portEXIT_CRITICAL(&ring->lock);
  return matched;
}

void capture_ring_record(CaptureRing *ring, const uint8_t *data, uint16_t length,
                         bool received) {
  if (!ring) return;
  // This runs from VHCI callbacks. Keep allocation and I/O outside the lock.
  portENTER_CRITICAL(&ring->lock);
  if (ring->active) {
    if (!data || !length || length > CAPTURE_H4_MAX) {
      ring->stats.invalid++;
    } else if (ring->count == CAPTURE_RING_CAPACITY) {
      ring->stats.full++;
    } else {
      CapturePacket *packet = &ring->packets[(ring->head + ring->count) %
                                               CAPTURE_RING_CAPACITY];
      packet->timestamp = ring->epoch + (esp_timer_get_time() - ring->origin);
      packet->sequence = ++ring->stats.accepted;
      packet->length = length;
      packet->received = received;
      memcpy(packet->bytes, data, length);
      if (++ring->count > ring->stats.peak) ring->stats.peak = ring->count;
    }
  }
  portEXIT_CRITICAL(&ring->lock);
}

CaptureEvent capture_ring_next(CaptureRing *ring, bool web, CapturePacket *packet,
                               CaptureStats *stats) {
  if (!ring || !packet || !stats) return CaptureNone;
  portENTER_CRITICAL(&ring->lock);
  CaptureEvent event = CaptureNone;
  if (ring->web != web) goto done;
  if (!web && ring->active && esp_timer_get_time() - ring->lease > 10000000) {
    ring->active = false;
    ring->ending = true;
  }
  *stats = ring->stats;
  if (ring->begin) {
    ring->begin = false;
    event = CaptureBegin;
  } else if (ring->count) {
    CapturePacket *source = &ring->packets[ring->head];
    memcpy(packet, source, offsetof(CapturePacket, bytes) + source->length);
    ring->head = (ring->head + 1) % CAPTURE_RING_CAPACITY;
    ring->count--;
    event = CaptureData;
  } else if (ring->ending && !ring->health_sent) {
    ring->health_sent = true;
    event = CaptureHealth;
  } else if (ring->ending) {
    ring->ending = false;
    ring->finished = true;
    event = CaptureEnd;
  }
done:
  portEXIT_CRITICAL(&ring->lock);
  return event;
}
