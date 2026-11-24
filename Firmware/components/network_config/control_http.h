#pragma once
#include "cJSON.h"
#include "esp_http_server.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool control_origin(httpd_req_t *request);
bool control_setup_ap_request(httpd_req_t *request);
bool control_access(httpd_req_t *request, bool allow_setup_ap);
bool control_settings_access(httpd_req_t *request);
esp_err_t control_json(httpd_req_t *request, cJSON *owned_json);
esp_err_t control_error(httpd_req_t *request, const char *status, const char *message);
cJSON *control_read(httpd_req_t *request);
esp_err_t network_http_register(httpd_handle_t server);
esp_err_t mcp_http_register(httpd_handle_t server);
#ifdef __cplusplus
}
#endif
