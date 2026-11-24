#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "App.h"
#include "capture_ring.h"
#include "capture/capture_store.h"
#include "capture/capture_export.h"
#include "capture/flipper_sd.h"
#include "ble/GattAttackApp.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "main"

void app_main(void) {
  ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
  gatt_app_init();
  CaptureRing *capture = capture_ring_create();
  capture_vhci_attach(capture);
  capture_store_init(capture);
  flipper_sd_init();
  capture_export_init();
  setup();

  while(true) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
