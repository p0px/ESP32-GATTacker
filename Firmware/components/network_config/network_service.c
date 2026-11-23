#include "network_service.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "network_config.h"
#include "nvs.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

// One boot-lifetime service. Event callbacks only update snapshots or wake its worker.
static NetworkConfig config;
static SemaphoreHandle_t mutex;
static TaskHandle_t worker;
static esp_netif_t *ap_netif;
static bool changed, scanning, scan_requested, station_up, ap_up;
static bool joining, associated;
static uint8_t station_mac[6];
static unsigned reason;
static char station_ip[16];
static wifi_ap_record_t networks[16];
static uint16_t network_count;
static esp_err_t last_error, scan_error;

static esp_err_t save(const NetworkConfig *value) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("network", NVS_READWRITE, &nvs);
  if (err != ESP_OK)
    return err;
  err = nvs_set_blob(nvs, "config", value, sizeof(*value));
  if (err == ESP_OK)
    err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

static void generate_key(char out[33]) {
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  for (size_t i = 0; i < sizeof(random); i++)
    snprintf(out + 2 * i, 3, "%02x", random[i]);
  memset(random, 0, sizeof(random));
}

static void event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *ip = data;
    snprintf(station_ip, sizeof(station_ip), IPSTR, IP2STR(&ip->ip_info.ip));
    station_up = true;
    joining = false;
    associated = true;
    reason = 0;
    last_error = ESP_OK;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    associated = true;
    joining = false;
    reason = 0;
    last_error = ESP_OK;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    station_up = false;
    associated = joining = false;
    station_ip[0] = 0;
    reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    ESP_LOGW("network", "Station disconnected, reason %u", reason);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
    ap_up = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
    ap_up = false;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    network_count = 16;
    scan_error = esp_wifi_scan_get_ap_records(&network_count, networks);
    if (scan_error != ESP_OK)
      network_count = 0;
    scanning = false;
  }
  xSemaphoreGive(mutex);
  if (worker)
    xTaskNotifyGive(worker);
}

static esp_err_t apply(const NetworkConfig *value) {
  esp_wifi_stop();
  wifi_config_t ap = {0}, sta = {0};
  memcpy(ap.ap.ssid, value->ap_ssid, strlen(value->ap_ssid));
  memcpy(ap.ap.password, value->ap_password, strlen(value->ap_password));
  ap.ap.ssid_len = strlen(value->ap_ssid);
  ap.ap.channel = 3;
  ap.ap.max_connection = 3;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.pmf_cfg.capable = true;
  memcpy(sta.sta.ssid, value->station_ssid, strlen(value->station_ssid));
  memcpy(sta.sta.password, value->station_password, strlen(value->station_password));
  sta.sta.pmf_cfg.capable = true;
  // APSTA during connection gives a recovery path even with incorrect credentials.
  esp_err_t err = esp_wifi_set_mode(!strcmp(value->mode, "ap") ? WIFI_MODE_AP : WIFI_MODE_APSTA);
  if (err == ESP_OK)
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
  if (err == ESP_OK && strcmp(value->mode, "ap"))
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
  if (err == ESP_OK)
    err = esp_wifi_start();
  return err;
}

static void run(void *arg) {
  (void)arg;
  int64_t next_connect = 0, attempt_started = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    xSemaphoreTake(mutex, portMAX_DELAY);
    NetworkConfig value = config;
    bool update = changed, scan = scan_requested, up = station_up;
    bool in_progress = joining || associated, scan_active = scanning;
    changed = scan_requested = false;
    if (update) {
      station_up = false;
      associated = joining = false;
      station_ip[0] = 0;
      reason = 0;
    }
    xSemaphoreGive(mutex);
    if (update) {
      // Allow the settings response to reach the client before changing its network.
      vTaskDelay(pdMS_TO_TICKS(300));
      esp_err_t err = apply(&value);
      xSemaphoreTake(mutex, portMAX_DELAY);
      last_error = err;
      scanning = false;
      xSemaphoreGive(mutex);
      up = false;
      in_progress = false;
      scan_active = false;
      next_connect = 0;
    }
    if (scan) {
      wifi_mode_t mode;
      esp_wifi_get_mode(&mode);
      if (mode == WIFI_MODE_AP)
        esp_wifi_set_mode(WIFI_MODE_APSTA);
      esp_err_t err = esp_wifi_scan_start(NULL, false);
      xSemaphoreTake(mutex, portMAX_DELAY);
      if (err != ESP_OK)
        scanning = false;
      scan_error = err;
      xSemaphoreGive(mutex);
    }
    if (strcmp(value.mode, "ap")) {
      wifi_mode_t desired = up && !strcmp(value.mode, "station") ? WIFI_MODE_STA : WIFI_MODE_APSTA;
      wifi_mode_t current;
      esp_wifi_get_mode(&current);
      if (current != desired)
        esp_wifi_set_mode(desired);
      int64_t now = esp_timer_get_time();
      if (!up && in_progress && now - attempt_started >= 30000000) {
        // Bound authentication/DHCP waits and retain a visible failure during retries.
        esp_wifi_disconnect();
        xSemaphoreTake(mutex, portMAX_DELAY);
        associated = joining = false;
        last_error = ESP_ERR_TIMEOUT;
        xSemaphoreGive(mutex);
        next_connect = now + 10000000;
      } else if (!up && !in_progress && !scan_active && !scan && now >= next_connect) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        joining = true;
        xSemaphoreGive(mutex);
        attempt_started = now;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
          xSemaphoreTake(mutex, portMAX_DELAY);
          last_error = err;
          joining = false;
          xSemaphoreGive(mutex);
          ESP_LOGW("network", "Station connect failed: %s", esp_err_to_name(err));
        }
        next_connect = now + 10000000;
      }
    }
    memset(&value, 0, sizeof(value));
  }
}

void network_init(void) {
  mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(mutex ? ESP_OK : ESP_ERR_NO_MEM);
  config.version = 1;
  strcpy(config.mode, "ap");
  snprintf(config.ap_ssid, sizeof(config.ap_ssid), "%s", WIFI_SSID);
  snprintf(config.ap_password, sizeof(config.ap_password), "%s", WIFI_PASS);
  nvs_handle_t nvs;
  if (nvs_open("network", NVS_READONLY, &nvs) == ESP_OK) {
    NetworkConfig stored;
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, "config", &stored, &size) == ESP_OK && size == sizeof(stored) &&
        stored.version == 1 && memchr(stored.mode, 0, sizeof(stored.mode)) &&
        memchr(stored.ap_ssid, 0, sizeof(stored.ap_ssid)) &&
        memchr(stored.ap_password, 0, sizeof(stored.ap_password)) &&
        memchr(stored.station_ssid, 0, sizeof(stored.station_ssid)) &&
        memchr(stored.station_password, 0, sizeof(stored.station_password)) &&
        stored.key[32] == 0) {
      cJSON *empty = cJSON_CreateObject();
      if (empty && !network_config_patch(&stored, empty))
        config = stored;
      cJSON_Delete(empty);
    }
    memset(&stored, 0, sizeof(stored));
    nvs_close(nvs);
  }
  if (!config.key[0]) {
    generate_key(config.key);
    ESP_ERROR_CHECK(save(&config));
  }
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ap_netif = esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();
  ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
  esp_netif_ip_info_t ip = {0};
  IP4_ADDR(&ip.ip, 1, 3, 3, 7);
  IP4_ADDR(&ip.gw, 1, 3, 3, 7);
  IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
  ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip));
  ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
  ESP_ERROR_CHECK(esp_read_mac(station_mac, ESP_MAC_WIFI_STA));
  if (!network_mac_valid(station_mac)) {
    // The inherited default 13:33:77:13:37:67 is multicast and cannot join normal APs.
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(station_mac));
    ESP_ERROR_CHECK(esp_iface_mac_addr_set(station_mac, ESP_MAC_WIFI_STA));
    ESP_LOGW("network", "Invalid station MAC replaced with the factory Wi-Fi MAC");
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL));
  changed = true;
  ESP_ERROR_CHECK(xTaskCreate(run, "network", 4096, NULL, 2, &worker) == pdPASS ? ESP_OK
                                                                                : ESP_ERR_NO_MEM);
  xTaskNotifyGive(worker);
}

const char *network_update(const cJSON *patch) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  NetworkConfig next = config;
  const char *error = network_config_patch(&next, patch);
  if (!error && save(&next) != ESP_OK)
    error = "Could not save Wi-Fi settings";
  if (!error) {
    // MCP toggles must not tear down Wi-Fi or an active BLE hook transport.
    changed |= strcmp(config.mode, next.mode) || strcmp(config.ap_ssid, next.ap_ssid) ||
               strcmp(config.ap_password, next.ap_password) ||
               strcmp(config.station_ssid, next.station_ssid) ||
               strcmp(config.station_password, next.station_password);
    config = next;
  }
  memset(&next, 0, sizeof(next));
  xSemaphoreGive(mutex);
  if (!error)
    xTaskNotifyGive(worker);
  return error;
}

const char *network_scan(void) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (scanning || changed) {
    xSemaphoreGive(mutex);
    return "Wi-Fi is busy; retry shortly";
  }
  scanning = scan_requested = true;
  scan_error = ESP_OK;
  xSemaphoreGive(mutex);
  xTaskNotifyGive(worker);
  return NULL;
}

void network_key(char out[33]) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  memcpy(out, config.key, 33);
  xSemaphoreGive(mutex);
}

const char *network_rotate_key(char out[33]) {
  out[0] = 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  NetworkConfig next = config;
  const char *error = "Could not generate a new access key";
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    generate_key(next.key);
    if (strcmp(next.key, config.key)) {
      error = NULL;
      break;
    }
  }
  if (!error && save(&next) != ESP_OK)
    error = "Could not save the new access key";
  if (!error) {
    config = next;
    memcpy(out, next.key, 33);
  }
  memset(&next, 0, sizeof(next));
  xSemaphoreGive(mutex);
  return error;
}

bool network_authorized(const char *token) {
  char key[33];
  network_key(key);
  unsigned difference = token && strlen(token) == 32 ? 0 : 1;
  if (!difference)
    for (size_t i = 0; i < 32; i++)
      difference |= token[i] ^ key[i];
  memset(key, 0, sizeof(key));
  return difference == 0;
}

bool network_mcp_enabled(void) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool enabled = config.mcp_enabled;
  xSemaphoreGive(mutex);
  return enabled;
}

void network_endpoint(char *out, size_t capacity) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  snprintf(out, capacity, "http://%s/mcp", station_up ? station_ip : "1.3.3.7");
  xSemaphoreGive(mutex);
}

cJSON *network_status(void) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON_AddStringToObject(json, "mode", config.mode);
  cJSON_AddStringToObject(json, "ap_ssid", config.ap_ssid);
  cJSON_AddStringToObject(json, "station_ssid", config.station_ssid);
  cJSON_AddBoolToObject(json, "station_password_set", config.station_password[0] != 0);
  cJSON_AddBoolToObject(json, "mcp_enabled", config.mcp_enabled);
  cJSON_AddBoolToObject(json, "ap_active", ap_up);
  cJSON_AddBoolToObject(json, "pending", changed);
  cJSON_AddStringToObject(json, "ap_ip", "1.3.3.7");
  cJSON_AddStringToObject(json, "station_ip", station_ip);
  char mac[18];
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", station_mac[0], station_mac[1],
           station_mac[2], station_mac[3], station_mac[4], station_mac[5]);
  cJSON_AddStringToObject(json, "station_mac", mac);
  const char *state = station_up                   ? "connected"
                      : !strcmp(config.mode, "ap") ? "ap"
                      : last_error == ESP_ERR_TIMEOUT ? "connection_timeout"
                      : last_error != ESP_OK ? "connection_failed"
                      : associated ? "waiting_for_ip"
                      : reason == WIFI_REASON_AUTH_FAIL ||
                              reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                              reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                          ? "authentication_failed"
                      : reason == WIFI_REASON_NO_AP_FOUND ? "network_not_found"
                      : reason ? "connection_failed"
                                                          : "connecting";
  cJSON_AddStringToObject(json, "state", state);
  cJSON_AddNumberToObject(json, "disconnect_reason", reason);
  cJSON_AddStringToObject(json, "error", last_error == ESP_OK ? "" : esp_err_to_name(last_error));
  cJSON_AddStringToObject(json, "scan_error", scan_error == ESP_OK ? "" :
                         esp_err_to_name(scan_error));
  cJSON_AddBoolToObject(json, "scanning", scanning);
  cJSON *list = cJSON_AddArrayToObject(json, "networks");
  for (unsigned i = 0; i < network_count; i++) {
    cJSON *item = cJSON_CreateObject();
    char ssid[33] = {0};
    memcpy(ssid, networks[i].ssid, 32);
    cJSON_AddStringToObject(item, "ssid", ssid);
    cJSON_AddNumberToObject(item, "rssi", networks[i].rssi);
    cJSON_AddBoolToObject(item, "secured", networks[i].authmode != WIFI_AUTH_OPEN);
    cJSON_AddItemToArray(list, item);
  }
  xSemaphoreGive(mutex);
  return json;
}
