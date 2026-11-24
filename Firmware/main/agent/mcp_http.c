#include "agent_api.h"
#include "control_http.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mcp_protocol.h"
#include "network_service.h"
#include <string.h>

static QueueHandle_t requests;

static void serve(void *arg) {
  (void)arg;
  httpd_req_t *req;
  for (;;) {
    if (xQueueReceive(requests, &req, portMAX_DELAY) != pdTRUE)
      continue;
    char version[24] = {0}, method[64] = {0}, name[128] = {0};
    httpd_req_get_hdr_value_str(req, "MCP-Protocol-Version", version, sizeof(version));
    httpd_req_get_hdr_value_str(req, "Mcp-Method", method, sizeof(method));
    httpd_req_get_hdr_value_str(req, "Mcp-Name", name, sizeof(name));
    size_t size = strlen(name);
    if (size >= 10 && !strncmp(name, "=?base64?", 8) && !strcmp(name + size - 2, "?=")) {
      unsigned char decoded[64];
      size_t used = 0;
      int err = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &used,
                                      (const unsigned char *)name + 8, size - 10);
      if (err || memchr(decoded, 0, used))
        name[0] = 0;
      else {
        memcpy(name, decoded, used);
        name[used] = 0;
      }
    }
    cJSON *body = control_read(req);
    const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(body, "method");
    if (strcmp(version, "2026-07-28") && cJSON_IsString(rpc) &&
        !strcmp(rpc->valuestring, "notifications/initialized") &&
        !cJSON_HasObjectItem(body, "id")) {
      httpd_resp_set_status(req, "202 Accepted");
      httpd_resp_send(req, NULL, 0);
    } else {
      McpHeaders headers = {version, method, name};
      bool bad;
      cJSON *reply = mcp_dispatch(body, &headers, &bad);
      if (bad)
        httpd_resp_set_status(req, "400 Bad Request");
      control_json(req, reply);
    }
    cJSON_Delete(body);
    httpd_req_async_handler_complete(req);
  }
}

static esp_err_t handle(httpd_req_t *req) {
  if (!control_origin(req))
    return control_error(req, "403 Forbidden", "Origin not allowed");
  if (!network_mcp_enabled())
    return control_error(req, "403 Forbidden", "Enable MCP in Settings");
  if (!control_access(req, false)) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"ESP32 GATTacker\"");
    return control_error(req, "401 Unauthorized", "MCP access key required");
  }
  if (req->method != HTTP_POST) {
    httpd_resp_set_hdr(req, "Allow", "POST");
    return control_error(req, "405 Method Not Allowed", "Use POST /mcp");
  }
  if (req->content_len > 2048)
    return control_error(req, "413 Content Too Large", "Limit: 2048 bytes");
  httpd_req_t *async;
  if (httpd_req_async_handler_begin(req, &async) != ESP_OK)
    return control_error(req, "503 Service Unavailable", "MCP is busy");
  if (xQueueSend(requests, &async, 0) != pdTRUE) {
    control_error(async, "503 Service Unavailable", "MCP queue full; retry later");
    httpd_req_async_handler_complete(async);
  }
  return ESP_OK;
}

esp_err_t mcp_http_register(httpd_handle_t server) {
  agent_init();
  requests = xQueueCreate(1, sizeof(httpd_req_t *));
  if (!requests || xTaskCreate(serve, "mcp", 6144, NULL, 2, NULL) != pdPASS)
    return ESP_ERR_NO_MEM;
  const httpd_uri_t post = {.uri = "/mcp", .method = HTTP_POST, .handler = handle};
  const httpd_uri_t get = {.uri = "/mcp", .method = HTTP_GET, .handler = handle};
  const httpd_uri_t del = {.uri = "/mcp", .method = HTTP_DELETE, .handler = handle};
  esp_err_t result = httpd_register_uri_handler(server, &post);
  if (result == ESP_OK)
    result = httpd_register_uri_handler(server, &get);
  return result == ESP_OK ? httpd_register_uri_handler(server, &del) : result;
}
