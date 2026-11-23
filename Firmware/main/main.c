#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "App.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "main"

void app_main(void) {
  ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
  setup();

  while(true) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
