#include "App.h"
#include "wifi/routes.h"
#include "wifi/wifi.h"
#include "dns_server.h"
#include "uart/uart_bridge.h"
#include "ble/traffic_store.h"

#define TAG "App"

void setup(void) {
  read_settings();
  traffic_store_init();
  uart_bridge_start();
  wifi_init_softap();
  dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
  start_dns_server(&config);
  ws_init();
  start_rest_server();

  if (ap_mac_address[0] & 0x01) {
    ESP_LOGE(TAG, "AP Might not show due to ap_mac_address[0] & 0x01");
  }

  ESP_LOGI(TAG, "HTTP server started");
}
