#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "App.h"
#include "capture_ring.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "main"

void app_main(void) {
  ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
  capture_vhci_attach(capture_ring_create());
  setup();

  while(true) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
