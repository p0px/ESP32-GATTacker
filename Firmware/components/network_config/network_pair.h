#pragma once
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

typedef bool (*NetworkPairConnected)(void *context);
typedef void (*NetworkPairRequest)(void *context, uint32_t id, const char *peer);

void network_pair_bind(NetworkPairConnected connected, NetworkPairRequest send,
                       void *context);
bool network_pair_decide(uint32_t id, bool allow);
esp_err_t network_pair_start_http(httpd_req_t *request);
esp_err_t network_pair_status_http(httpd_req_t *request);
bool network_pair_authorized(httpd_req_t *request, const char *ticket);
