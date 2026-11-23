#pragma once

#include "cJSON.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// On success the caller owns frame->payload, which may be NULL for an empty frame.
esp_err_t ws_read_frame(httpd_req_t *req, httpd_ws_frame_t *frame);
void ws_wifi_response(const cJSON *request, cJSON *response);
void ws_reboot_response(const cJSON *request, cJSON *response);

#ifdef __cplusplus
}
#endif
