#include "ws_request.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "settings.h"

enum { MAX_WS_FRAME = 4096 };

esp_err_t ws_read_frame(httpd_req_t *req, httpd_ws_frame_t *frame) {
  *frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_TEXT};
  esp_err_t result = httpd_ws_recv_frame(req, frame, 0);
  if (result != ESP_OK) {
    ESP_LOGE("WS", "Failed to get frame length: %d", result);
    return result;
  }
  if (frame->len > MAX_WS_FRAME) return ESP_ERR_INVALID_SIZE;
  if (!frame->len) return ESP_OK;

  // cJSON consumes a NUL-terminated string. Binary frames keep their original length.
  frame->payload = calloc(frame->len + 1, 1);
  if (!frame->payload) return ESP_ERR_NO_MEM;
  result = httpd_ws_recv_frame(req, frame, frame->len);
  if (result != ESP_OK) {
    ESP_LOGE("WS", "Failed to receive frame: %d", result);
    free(frame->payload);
    frame->payload = NULL;
  }
  return result;
}

static void error_response(cJSON *response, const char *message) {
  cJSON_AddBoolToObject(response, "success", false);
  cJSON_AddStringToObject(response, "error", message);
}

void ws_wifi_response(const cJSON *request, cJSON *response) {
  const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(request, "ssid"));
  const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(request, "pass"));
  if (!ssid || !pass) {
    error_response(response, "Invalid Params");
    return;
  }
  if (!*ssid) {
    error_response(response, "SSID cannot be blank");
    return;
  }
  if (strlen(pass) < 8) {
    error_response(response, "PASS cannot be blank and must be longer than 8 characters");
    return;
  }

  esp_err_t result = write_string_to_nvs("wifi_ssid", ssid);
  if (result == ESP_OK) {
    strlcpy(WIFI_SSID, ssid, sizeof(WIFI_SSID));
    result = write_string_to_nvs("wifi_pass", pass);
    if (result == ESP_OK) strlcpy(WIFI_PASS, pass, sizeof(WIFI_PASS));
  }
  if (result != ESP_OK) {
    error_response(response, esp_err_to_name(result));
    return;
  }
  cJSON_AddBoolToObject(response, "success", true);
}

void ws_reboot_response(const cJSON *request, cJSON *response) {
  const cJSON *value = cJSON_GetObjectItem(request, "value");
  if (cJSON_IsNumber(value) && value->valueint > 9000) esp_restart();
  error_response(response, "Invalid Value");
}
