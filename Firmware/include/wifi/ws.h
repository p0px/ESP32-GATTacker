#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int num_clients;
void msg_clients(const char *msg);
bool ws_has_clients(void);
bool try_msg_clients(const char *msg, bool hook_request);
void ws_init(void);
void ws_on_close_handler(httpd_handle_t hd, int sockfd);
esp_err_t ws_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
