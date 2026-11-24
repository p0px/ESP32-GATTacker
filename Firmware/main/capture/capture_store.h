#pragma once

#include "capture_ring.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  CaptureEmpty,
  CaptureRecording,
  CaptureSaving,
  CaptureReady,
} CaptureStoreState;

typedef struct {
  bool available;
  bool downloading;
  bool auto_export;
  bool export_failed;
  bool flipper;
  CaptureStoreState state;
  uint32_t id;
  uint32_t packets;
  uint32_t dropped;
  uint32_t full;
  uint32_t invalid;
  uint32_t peak;
  size_t bytes;
  size_t capacity;
  char reason[96];
} CaptureStoreStatus;

typedef struct {
  const uint8_t *bytes;
  size_t size;
  uint32_t id;
  uint32_t packets;
  uint32_t dropped;
} CaptureDownload;

void capture_store_init(CaptureRing *ring);
CaptureStoreStatus capture_store_status(void);
const char *capture_store_start(uint32_t seconds, bool auto_export, bool flipper_clock);
const char *capture_store_stop(uint32_t id);
const char *capture_store_clear(uint32_t id);
bool capture_store_download(CaptureDownload *download);
void capture_store_release(void);
CaptureRing *capture_store_ring(void);
void capture_store_export_failed(uint32_t id);
