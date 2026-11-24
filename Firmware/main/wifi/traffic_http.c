#include "wifi/traffic_http.h"
#include "ble/traffic_store.h"
#include "control_http.h"
#include "wire.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static esp_err_t traffic_read_http(httpd_req_t *request) {
  uint32_t after = 0;
  uint32_t limit = 64;
  char query[80], token[16];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "after", token, sizeof(token)) == ESP_OK &&
        !wire_number(token, UINT32_MAX, &after))
      return control_error(request, "400 Bad Request", "Invalid traffic cursor");
    if (httpd_query_key_value(query, "limit", token, sizeof(token)) == ESP_OK &&
        !wire_number(token, 64, &limit))
      return control_error(request, "400 Bad Request", "Invalid traffic limit");
  }
  if (!limit) return control_error(request, "400 Bad Request", "Traffic limit must be positive");
  TrafficPosition position = traffic_store_position();
  if (!position.capacity)
    return control_error(request, "503 Service Unavailable", "Traffic buffer unavailable");
  bool gap = after > position.newest || after < position.oldest - 1;
  uint32_t cursor = gap ? position.oldest - 1 : after;
  cJSON *result = cJSON_CreateObject();
  if (!result) return control_error(request, "503 Service Unavailable", "Out of memory");
  cJSON *events = cJSON_AddArrayToObject(result, "events");
  if (!events) {
    cJSON_Delete(result);
    return control_error(request, "503 Service Unavailable", "Out of memory");
  }
  while (cursor < position.newest && limit--) {
    TrafficEvent event;
    ++cursor;
    if (!traffic_store_read(cursor, &event)) {
      gap = true;
      continue;
    }
    cJSON *item = cJSON_CreateObject();
    if (!item) {
      cJSON_Delete(result);
      return control_error(request, "503 Service Unavailable", "Out of memory");
    }
    cJSON_AddNumberToObject(item, "cursor", event.cursor);
    cJSON_AddNumberToObject(item, "time_ms", event.time_ms);
    cJSON_AddNumberToObject(item, "trans_id", event.transaction);
    cJSON_AddNumberToObject(item, "status", event.status);
    cJSON_AddNumberToObject(item, "handle", event.handle);
    cJSON_AddBoolToObject(item, "truncated", event.truncated);
    cJSON_AddStringToObject(item, "operation", event.operation);
    cJSON_AddStringToObject(item, "direction", event.direction);
    cJSON_AddStringToObject(item, "kind", event.kind);
    cJSON_AddStringToObject(item, "uuid", event.uuid);
    if (event.has_data) cJSON_AddStringToObject(item, "data", event.data);
    cJSON_AddItemToArray(events, item);
  }
  cJSON_AddBoolToObject(result, "gap", gap);
  cJSON_AddBoolToObject(result, "has_more", cursor < position.newest);
  cJSON_AddNumberToObject(result, "next_cursor", cursor);
  cJSON_AddNumberToObject(result, "newest", position.newest);
  cJSON_AddNumberToObject(result, "uptime_ms", position.uptime_ms);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return control_json(request, result);
}

esp_err_t traffic_http_register(httpd_handle_t server) {
  const httpd_uri_t route = {
    .uri = "/api/traffic",
    .method = HTTP_GET,
    .handler = traffic_read_http,
  };
  return httpd_register_uri_handler(server, &route);
}
