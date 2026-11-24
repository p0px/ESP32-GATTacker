#include "capture/capture_store.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcap.h"
#include <stdio.h>
#include <string.h>

static const size_t MAX_CAPACITY = 4 * 1024 * 1024;
static const size_t MIN_CAPACITY = 2 * 1024 * 1024;

typedef struct {
  SemaphoreHandle_t mutex;
  CaptureRing *ring;
  CaptureStoreStatus status;
  uint8_t *data;
  uint64_t timestamp;
  uint32_t omitted;
  CapturePacket packet;
  uint8_t block[PCAP_BUFFER_SIZE];
} CaptureStore;

static CaptureStore store;

static void update_stats(const CaptureStats *stats) {
  store.status.full = stats->full;
  store.status.invalid = stats->invalid;
  store.status.peak = stats->peak;
  store.status.dropped = stats->full + stats->invalid + store.omitted;
}

static void stop(const char *reason) {
  if (store.status.state != CaptureRecording) return;
  capture_ring_stop(store.ring, store.status.id, true);
  store.status.state = CaptureSaving;
  snprintf(store.status.reason, sizeof(store.status.reason), "%s", reason);
}

static void append(size_t length) {
  memcpy(store.data + store.status.bytes, store.block, length);
  store.status.bytes += length;
}

static void finish(const CaptureStats *stats) {
  update_stats(stats);
  store.status.dropped = stats->full + stats->invalid +
                         stats->accepted - store.status.packets;
  char detail[240];
  snprintf(detail, sizeof(detail),
           "ESP web recorder; full=%lu invalid=%lu unsaved=%lu queue peak=%u/%u. %s",
           (unsigned long)stats->full, (unsigned long)stats->invalid,
           (unsigned long)(stats->accepted - store.status.packets), stats->peak,
           CAPTURE_RING_CAPACITY, store.status.reason);
  append(pcap_statistics(store.block, store.timestamp, store.status.packets,
                         store.status.dropped, true, detail));
  store.status.state = CaptureReady;
}

static void poll(void) {
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  if (store.status.state == CaptureRecording || store.status.state == CaptureSaving) {
    for (unsigned i = 0; i < CAPTURE_RING_CAPACITY; i++) {
      CaptureStats stats;
      CaptureEvent event = capture_ring_next(store.ring, true, &store.packet, &stats);
      if (event == CaptureNone) break;
      if (event == CaptureEnd) {
        finish(&stats);
        break;
      }
      if (event != CaptureData) continue;
      size_t length = pcap_packet(store.block, store.packet.timestamp,
                                  store.packet.received, store.packet.bytes,
                                  store.packet.length);
      if (length && store.status.bytes + length + PCAP_BUFFER_SIZE <=
          store.status.capacity) {
        append(length);
        store.status.packets++;
        store.timestamp = store.packet.timestamp;
      } else {
        store.omitted++;
        if (length) stop("Memory limit reached; download this capture to continue.");
      }
      update_stats(&stats);
    }
  }
  xSemaphoreGive(store.mutex);
}

static void worker(void *context) {
  (void)context;
  for (;;) {
    poll();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void capture_store_init(CaptureRing *ring) {
  if (store.mutex) return;
  store.ring = ring;
  store.status.available = ring != NULL;
  store.status.capacity = MAX_CAPACITY;
  store.mutex = xSemaphoreCreateMutex();
  if (!store.mutex) store.status.available = false;
  if (store.mutex && ring &&
      xTaskCreate(worker, "pcap_store", 6144, NULL, 2, NULL) != pdPASS)
    store.status.available = false;
}

CaptureStoreStatus capture_store_status(void) {
  CaptureStoreStatus status = {0};
  if (!store.mutex) return status;
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  status = store.status;
  if (store.ring) {
    portENTER_CRITICAL(&store.ring->lock);
    status.flipper = !store.ring->web &&
                     (store.ring->active || store.ring->ending || store.ring->begin);
    if (store.ring->web && store.ring->stats.token == status.id &&
        (status.state == CaptureRecording || status.state == CaptureSaving)) {
      status.full = store.ring->stats.full;
      status.invalid = store.ring->stats.invalid;
      status.peak = store.ring->stats.peak;
      status.dropped = status.full + status.invalid + store.omitted;
    }
    portEXIT_CRITICAL(&store.ring->lock);
  }
  xSemaphoreGive(store.mutex);
  return status;
}

const char *capture_store_start(uint32_t seconds, bool auto_export, bool flipper_clock) {
  if (!store.mutex || !store.status.available) return "Web capture requires PSRAM";
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  if (store.status.state != CaptureEmpty || store.status.downloading) {
    xSemaphoreGive(store.mutex);
    return "Download and discard the previous capture before starting another";
  }
  size_t capacity = MAX_CAPACITY;
  uint8_t *data = NULL;
  for (; capacity >= MIN_CAPACITY; capacity -= 1024 * 1024) {
    data = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data) break;
  }
  if (!data) {
    xSemaphoreGive(store.mutex);
    return "Not enough PSRAM for a 2 MiB recording";
  }
  uint32_t id = esp_random() | 1;
  const char *error = capture_ring_begin(store.ring, id, seconds, true);
  if (error) {
    heap_caps_free(data);
    xSemaphoreGive(store.mutex);
    return error;
  }
  store.data = data;
  store.status = (CaptureStoreStatus){.available = true, .auto_export = auto_export,
                                      .id = id, .capacity = capacity,
                                      .state = CaptureRecording};
  store.timestamp = (uint64_t)seconds * 1000000;
  store.omitted = 0;
  append(pcap_header_clock(store.block, flipper_clock ? "Flipper RTC (1 s accuracy)" :
                          "browser UTC (1 s accuracy)"));
  xSemaphoreGive(store.mutex);
  return NULL;
}

const char *capture_store_stop(uint32_t id) {
  if (!store.mutex) return "Web capture unavailable";
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  const char *error = NULL;
  if (!id || store.status.id != id) error = "Capture changed; refresh its status";
  else stop("Stopped by user.");
  xSemaphoreGive(store.mutex);
  return error;
}

const char *capture_store_clear(uint32_t id) {
  if (!store.mutex) return "Web capture unavailable";
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  const char *error = NULL;
  if (!id || store.status.id != id) error = "Capture changed; refresh its status";
  else if (store.status.state != CaptureReady || store.status.downloading)
    error = "Stop recording and finish downloading before discarding";
  else {
    heap_caps_free(store.data);
    store.data = NULL;
    store.status = (CaptureStoreStatus){.available = true, .capacity = MAX_CAPACITY};
  }
  xSemaphoreGive(store.mutex);
  return error;
}

bool capture_store_download(CaptureDownload *download) {
  if (!store.mutex || !download) return false;
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  bool ready = store.status.state == CaptureReady && !store.status.downloading;
  if (ready) {
    store.status.downloading = true;
    *download = (CaptureDownload){store.data, store.status.bytes, store.status.id,
                                  store.status.packets, store.status.dropped};
  }
  xSemaphoreGive(store.mutex);
  return ready;
}

void capture_store_release(void) {
  if (!store.mutex) return;
  xSemaphoreTake(store.mutex, portMAX_DELAY);
  store.status.downloading = false;
  xSemaphoreGive(store.mutex);
}
