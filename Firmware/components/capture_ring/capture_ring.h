#pragma once

#include "capture_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CAPTURE_RING_CAPACITY = 256 };

typedef struct {
  uint64_t timestamp;
  uint32_t sequence;
  uint16_t length;
  bool received;
  uint8_t bytes[CAPTURE_H4_MAX];
} CapturePacket;

typedef struct {
  uint32_t token;
  uint32_t accepted;
  uint32_t full;
  uint32_t invalid;
  unsigned peak;
} CaptureStats;

typedef enum {
  CaptureNone,
  CaptureBegin,
  CaptureData,
  CaptureHealth,
  CaptureEnd,
} CaptureEvent;

typedef struct {
  portMUX_TYPE lock;
  CaptureStats stats;
  CapturePacket *packets;
  unsigned head;
  unsigned count;
  bool active;
  bool web;
  bool begin;
  bool ending;
  bool health_sent;
  bool finished;
  int64_t origin;
  int64_t lease;
  uint64_t epoch;
} CaptureRing;

CaptureRing *capture_ring_create(void);
void capture_ring_destroy(CaptureRing *ring);
const char *capture_ring_begin(CaptureRing *ring, uint32_t token, uint32_t seconds,
                               bool web);
bool capture_ring_stop(CaptureRing *ring, uint32_t token, bool web);
bool capture_ring_keepalive(CaptureRing *ring, uint32_t token);
void capture_ring_record(CaptureRing *ring, const uint8_t *data, uint16_t length,
                         bool received);
CaptureEvent capture_ring_next(CaptureRing *ring, bool web, CapturePacket *packet,
                               CaptureStats *stats);
