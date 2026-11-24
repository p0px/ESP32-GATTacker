#include "control_http.h"
#include "network_service.h"
#include "network_pair.h"
#include <string.h>

static esp_err_t settings(httpd_req_t *req) {
  if (!control_settings_access(req))
    return control_error(req, "401 Unauthorized", "Approve on Flipper or enter the access key");
  if (req->method == HTTP_POST) {
    cJSON *patch = control_read(req);
    const char *error = patch ? network_update(patch) : "Invalid settings JSON (max 2048 bytes)";
    cJSON_Delete(patch);
    if (error)
      return control_error(req, "400 Bad Request", error);
  }
  return control_json(req, network_status());
}

static esp_err_t scan(httpd_req_t *req) {
  if (!control_settings_access(req))
    return control_error(req, "401 Unauthorized", "Access key required");
  const char *error = network_scan();
  if (error)
    return control_error(req, "409 Conflict", error);
  return control_json(req, network_status());
}

static esp_err_t key(httpd_req_t *req) {
  if (!control_settings_access(req))
    return control_error(req, "401 Unauthorized", "Access key required");
  char token[33];
  network_key(token);
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    memset(token, 0, sizeof(token));
    return control_error(req, "503 Service Unavailable", "Out of memory");
  }
  if (!cJSON_AddStringToObject(json, "key", token)) {
    cJSON_Delete(json);
    memset(token, 0, sizeof(token));
    return control_error(req, "503 Service Unavailable", "Out of memory");
  }
  memset(token, 0, sizeof(token));
  return control_json(req, json);
}

static esp_err_t rotate_key(httpd_req_t *req) {
  if (!control_settings_access(req))
    return control_error(req, "401 Unauthorized", "Approve on Flipper or enter the access key");
  if (req->content_len)
    return control_error(req, "400 Bad Request", "Key rotation request must be empty");
  char token[33];
  const char *error = network_rotate_key(token);
  if (error) return control_error(req, "500 Internal Server Error", error);
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    memset(token, 0, sizeof(token));
    return control_error(req, "503 Service Unavailable", "Out of memory");
  }
  if (!cJSON_AddStringToObject(json, "key", token)) {
    cJSON_Delete(json);
    memset(token, 0, sizeof(token));
    return control_error(req, "503 Service Unavailable", "Out of memory");
  }
  memset(token, 0, sizeof(token));
  return control_json(req, json);
}

esp_err_t network_http_register(httpd_handle_t server) {
  const httpd_uri_t routes[] = {
      {.uri = "/api/network", .method = HTTP_GET, .handler = settings},
      {.uri = "/api/network", .method = HTTP_POST, .handler = settings},
      {.uri = "/api/network/scan", .method = HTTP_POST, .handler = scan},
      {.uri = "/api/network/key", .method = HTTP_GET, .handler = key},
      {.uri = "/api/network/key/rotate", .method = HTTP_POST, .handler = rotate_key},
      {.uri = "/api/network/pair/start", .method = HTTP_POST,
       .handler = network_pair_start_http},
      {.uri = "/api/network/pair/status", .method = HTTP_POST,
       .handler = network_pair_status_http},
  };
  for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    esp_err_t error = httpd_register_uri_handler(server, &routes[i]);
    if (error != ESP_OK)
      return error;
  }
  return ESP_OK;
}
