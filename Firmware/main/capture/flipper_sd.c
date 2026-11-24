#include "capture/flipper_sd.h"
#include "capture/capture_store.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uart/uart_bridge.h"
#include "wire.h"
#include <stdio.h>
#include <string.h>

typedef struct {
  SemaphoreHandle_t mutex;
  FlipperSdStatus status;
  int64_t seen;
  int64_t requested;
  uint32_t pending;
} FlipperSd;

static FlipperSd sink;

static bool active(const char *state) {
  return !strcmp(state, "starting") || !strcmp(state, "recording") ||
         !strcmp(state, "saving");
}

static bool valid_path(const char *path) {
  if (!*path) return true;
  const char *prefix = "/ext/apps_data/gattacker/capture-";
  if (strncmp(path, prefix, strlen(prefix)) || strlen(path) >= 128) return false;
  return strspn(path, "abcdefghijklmnopqrstuvwxyz0123456789/_.-") == strlen(path);
}

void flipper_sd_init(void) {
  if (sink.mutex) return;
  sink.mutex = xSemaphoreCreateMutex();
  snprintf(sink.status.state, sizeof(sink.status.state), "empty");
}

FlipperSdStatus flipper_sd_status(void) {
  FlipperSdStatus status = {0};
  if (!sink.mutex) return status;
  xSemaphoreTake(sink.mutex, portMAX_DELAY);
  int64_t now = esp_timer_get_time();
  if (sink.pending && now - sink.requested > 6000000) {
    sink.pending = 0;
    snprintf(sink.status.error, sizeof(sink.status.error),
             "Flipper did not confirm the request. Check its screen before retrying.");
    snprintf(sink.status.state, sizeof(sink.status.state), "error");
  }
  status = sink.status;
  status.connected = sink.seen && now - sink.seen < 5000000 && uart_bridge_connected();
  status.available = status.available && status.connected;
  xSemaphoreGive(sink.mutex);
  return status;
}

bool flipper_sd_receive(char **f, size_t count) {
  bool report = !strcmp(f[0], "capture_sink");
  bool failure = !strcmp(f[0], "capture_sink_error");
  if (!report && !failure) return false;
  if (!sink.mutex) return true;
  xSemaphoreTake(sink.mutex, portMAX_DELAY);
  if (failure && count == 3) {
    uint32_t id, code;
    if (wire_number(f[1], UINT32_MAX, &id) && wire_number(f[2], 3, &code) &&
        id && id == sink.pending) {
      sink.pending = 0;
      const char *message = code == 1 ? "Flipper is busy or capture changed." :
                            code == 2 ? "Flipper SD card unavailable." :
                            "Flipper could not start recording.";
      snprintf(sink.status.error, sizeof(sink.status.error), "%s", message);
      snprintf(sink.status.state, sizeof(sink.status.state), "error");
    }
  } else if (report && count == 10 && !strcmp(f[1], "1")) {
    const char *states[] = {"empty", "starting", "recording", "saving", "ready", "error"};
    bool valid = false;
    for (unsigned i = 0; i < 6; i++) valid |= !strcmp(states[i], f[4]);
    uint32_t available, id, packets, dropped, bytes, complete;
    valid &= valid_path(f[9]) && wire_number(f[2], 1, &available) &&
             wire_number(f[3], UINT32_MAX, &id) &&
             wire_number(f[5], UINT32_MAX, &packets) &&
             wire_number(f[6], UINT32_MAX, &dropped) &&
             wire_number(f[7], UINT32_MAX, &bytes) &&
             wire_number(f[8], 1, &complete);
    if (valid) {
      sink.seen = esp_timer_get_time();
      sink.status.available = available;
      if (!sink.pending || id == sink.pending) {
        sink.pending = 0;
        if (id != sink.status.id && active(f[4])) sink.status.error[0] = 0;
        sink.status.id = id;
        sink.status.packets = packets;
        sink.status.dropped = dropped;
        sink.status.bytes = bytes;
        sink.status.complete = complete;
        snprintf(sink.status.state, sizeof(sink.status.state), "%s", f[4]);
        snprintf(sink.status.path, sizeof(sink.status.path), "%s", f[9]);
        if (!strcmp(f[4], "error") && !sink.status.error[0])
          snprintf(sink.status.error, sizeof(sink.status.error),
                   "Recording interrupted. Check Flipper and saved file.");
      }
    }
  }
  xSemaphoreGive(sink.mutex);
  return true;
}

const char *flipper_sd_request(bool start, uint32_t id) {
  if (!capture_store_ring()) return "Board capture is unavailable";
  FlipperSdStatus status = flipper_sd_status();
  if (!status.connected) return "Open GATTacker on the connected Flipper";
  if (start && !status.available) return "Flipper SD card is unavailable";
  char event[64];
  xSemaphoreTake(sink.mutex, portMAX_DELAY);
  if (sink.pending) {
    xSemaphoreGive(sink.mutex);
    return "A Flipper capture request is already pending";
  }
  if (start) {
    CaptureRing *ring = capture_store_ring();
    portENTER_CRITICAL(&ring->lock);
    bool busy = active(sink.status.state) || ring->active || ring->ending || ring->begin;
    portEXIT_CRITICAL(&ring->lock);
    if (busy) {
      xSemaphoreGive(sink.mutex);
      return "Stop the current recording first";
    }
    id = esp_random() | 1;
    sink.status = (FlipperSdStatus){.available = true, .id = id};
  } else {
    if (!id || id != sink.status.id) {
      xSemaphoreGive(sink.mutex);
      return "Flipper capture changed; refresh status";
    }
    if (!active(sink.status.state)) {
      xSemaphoreGive(sink.mutex);
      return NULL;
    }
  }
  sink.status.error[0] = 0;
  sink.pending = id;
  sink.requested = esp_timer_get_time();
  snprintf(sink.status.state, sizeof(sink.status.state), "%s", start ? "starting" : "saving");
  snprintf(event, sizeof(event), "capture_remote\t%s\t%lu", start ? "start" : "stop",
           (unsigned long)id);
  xSemaphoreGive(sink.mutex);
  uart_bridge_event(event);
  return NULL;
}
