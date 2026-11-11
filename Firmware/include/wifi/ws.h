#ifndef WS_H
#define WS_H

#include <string>
#include <sys/param.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_netif.h"
#include "cJSON.h"

#include "ble/GattAttackApp.h"

extern int num_clients;
void ws_init(void);
void msg_clients(const std::string &msg);
void msg_clients(const char *msg);
bool ws_has_clients();
bool try_msg_clients(const char *msg, bool hook_request = false);
void ws_on_close_handler(httpd_handle_t hd, int sockfd);
esp_err_t ws_handler(httpd_req_t *req);

#endif
