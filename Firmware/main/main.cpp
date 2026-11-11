#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "App.h"
#include "esp_system.h"

#define TAG "main"

extern "C" void app_main() {
  ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
  setup();

  while(true) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}