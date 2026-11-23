#ifndef SETTINGS_H
#define SETTINGS_H

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"

#define MAC_LEN 6

#ifdef __cplusplus
extern "C" {
#endif

extern const char* version;
extern const char* BASE_PATH;

// WiFi AP & Webserver
extern char WIFI_SSID[32];
extern char WIFI_PASS[32];

typedef void (*AllClientsDisconnectedCallback)(void);

extern uint8_t bt_mac_address[MAC_LEN];
extern uint8_t ap_mac_address[MAC_LEN];
extern uint8_t st_mac_address[MAC_LEN];
extern const uint8_t default_bt_mac_address[MAC_LEN];
extern const uint8_t default_ap_mac_address[MAC_LEN];
extern const uint8_t default_st_mac_address[MAC_LEN];

// WIFI (No setup)
#define DEFAULT_WIFI_SSID "ESP_WIFI"
#define DEFAULT_WIFI_PASS "gattattack"

// nvs stuff
esp_err_t init_nvs(void);
esp_err_t write_string_to_nvs(const char* key, const char* value);
esp_err_t read_string_from_nvs(const char* key, char* buffer, size_t buffer_size);
esp_err_t write_uint32_to_nvs(const char* key, uint32_t value);
esp_err_t read_uint32_from_nvs(const char* key, uint32_t* value);
esp_err_t write_blob_to_nvs(const char* key, const void* data, size_t length);
esp_err_t read_blob_from_nvs(const char* key, void* data, size_t length);
void read_settings(void);

// LEDs
#define IDLETIMER3 3 * 1000000 // 3 seconds
#define IDLETIMER 10 * 1000000 // 10 seconds
#define LED_STRIP_BLINK_GPIO  38
#define LED_STRIP_LED_NUMBERS 1
#define LED_STRIP_RMT_RES_HZ  (10 * 1000 * 1000)

extern esp_ble_scan_params_t default_scan_params;
extern esp_ble_adv_params_t default_adv_params;

#ifdef __cplusplus
}
#endif

#endif // SETTINGS_H
