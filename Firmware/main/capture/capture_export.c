#include "capture/capture_export.h"
#include "capture/capture_store.h"
#include "capture/flipper_sd.h"
#include "capture_codec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "uart/uart_bridge.h"
#include "wire.h"
#include <stdio.h>
#include <string.h>

enum { CHUNK = 900 };

typedef struct {
  SemaphoreHandle_t mutex;
  CaptureDownload download;
  uint32_t acknowledged;
  CaptureExportStatus status;
} ExportJob;

static ExportJob job;

static uint32_t checksum(uint32_t value, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) value = (value ^ data[i]) * 16777619U;
  return value;
}

static bool wait_ack(uint32_t expected) {
  int64_t until = esp_timer_get_time() + 2000000;
  while (esp_timer_get_time() < until) {
    xSemaphoreTake(job.mutex, portMAX_DELAY);
    bool received = job.acknowledged == expected;
    xSemaphoreGive(job.mutex);
    if (received) return true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

static bool send_step(const char *event, uint32_t expected) {
  for (unsigned attempt = 0; attempt < 4; attempt++) {
    uart_bridge_event(event);
    if (wait_ack(expected)) return true;
  }
  return false;
}

static void export_task(void *context) {
  (void)context;
  char event[WIRE_MAX];
  uint32_t id = job.download.id;
  size_t length = job.download.size;
  uint32_t hash = 2166136261U;
  snprintf(event, sizeof(event), "capture_export_begin\t%lu\t%u\t%lu\t%lu",
           (unsigned long)id, (unsigned)length,
           (unsigned long)job.download.packets, (unsigned long)job.download.dropped);
  bool success = send_step(event, 0);
  for (size_t offset = 0; success && offset < length; offset += CHUNK) {
    size_t count = length - offset < CHUNK ? length - offset : CHUNK;
    int prefix = snprintf(event, sizeof(event), "capture_export_data\t%lu\t%u\t",
                          (unsigned long)id, (unsigned)offset);
    if (prefix <= 0 || (size_t)prefix >= sizeof(event) ||
        !capture_base64(job.download.bytes + offset, count, event + prefix,
                        sizeof(event) - prefix)) {
      success = false;
      break;
    }
    success = send_step(event, (uint32_t)(offset + count));
    if (success) {
      hash = checksum(hash, job.download.bytes + offset, count);
      xSemaphoreTake(job.mutex, portMAX_DELAY);
      job.status.bytes = offset + count;
      xSemaphoreGive(job.mutex);
    }
  }
  if (success) {
    snprintf(event, sizeof(event), "capture_export_end\t%lu\t%u\t%lu",
             (unsigned long)id, (unsigned)length, (unsigned long)hash);
    success = send_step(event, (uint32_t)length + 1);
  }
  if (!success) {
    snprintf(event, sizeof(event), "capture_export_abort\t%lu", (unsigned long)id);
    uart_bridge_event(event);
  }
  capture_store_release();
  xSemaphoreTake(job.mutex, portMAX_DELAY);
  job.status.active = false;
  snprintf(job.status.state, sizeof(job.status.state), "%s",
           success ? "complete" : "failed");
  xSemaphoreGive(job.mutex);
  vTaskDelete(NULL);
}

void capture_export_init(void) {
  if (job.mutex) return;
  job.mutex = xSemaphoreCreateMutex();
  snprintf(job.status.state, sizeof(job.status.state), "idle");
}

bool capture_export_receive(char **fields, size_t count) {
  if (strcmp(fields[0], "capture_export_ack")) return false;
  uint32_t id, offset;
  if (job.mutex && count == 3 && wire_number(fields[1], UINT32_MAX, &id) &&
      wire_number(fields[2], UINT32_MAX, &offset)) {
    xSemaphoreTake(job.mutex, portMAX_DELAY);
    if (job.status.active && id == job.download.id) job.acknowledged = offset;
    xSemaphoreGive(job.mutex);
  }
  return true;
}

const char *capture_export_start(uint32_t id) {
  if (!job.mutex) return "SD export unavailable";
  if (!flipper_sd_status().available)
    return "Open GATTacker on a Flipper with an SD card";
  xSemaphoreTake(job.mutex, portMAX_DELAY);
  if (job.status.active) {
    xSemaphoreGive(job.mutex);
    return "SD export already running";
  }
  if (!capture_store_download(&job.download)) {
    xSemaphoreGive(job.mutex);
    return "Stop the RAM capture before exporting";
  }
  if (job.download.id != id) {
    capture_store_release();
    xSemaphoreGive(job.mutex);
    return "Capture changed; refresh status";
  }
  job.acknowledged = UINT32_MAX;
  job.status = (CaptureExportStatus){.active = true, .id = id};
  snprintf(job.status.state, sizeof(job.status.state), "transferring");
  if (xTaskCreate(export_task, "sd_export", 8192, NULL, 2, NULL) != pdPASS) {
    job.status.active = false;
    snprintf(job.status.state, sizeof(job.status.state), "failed");
    capture_store_release();
    xSemaphoreGive(job.mutex);
    return "SD export worker unavailable";
  }
  xSemaphoreGive(job.mutex);
  return NULL;
}

CaptureExportStatus capture_export_status(void) {
  CaptureExportStatus status = {0};
  if (!job.mutex) return status;
  xSemaphoreTake(job.mutex, portMAX_DELAY);
  status = job.status;
  xSemaphoreGive(job.mutex);
  return status;
}
