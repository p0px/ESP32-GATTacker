#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "App.h"
#include "capture_ring.h"
#include "capture/capture_store.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "main"

void app_main(void) {
  ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
  CaptureRing *capture = capture_ring_create();
  capture_vhci_attach(capture);
  capture_store_init(capture);
  setup();

  while(true) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
