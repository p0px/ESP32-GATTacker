#include "wifi/ws.h"
#include "ws_request.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "ble/GattAttackApp.h"
#include "uart/uart_bridge.h"
#include <unistd.h>
#include "lwip/sockets.h"

#define TAG "WS"

typedef enum {
  WS_OPT_SET_WIFI,
  WS_OPT_REBOOT,
  WS_OPT_GATTATTACK,
  WS_OPT_BOARD_STATUS = 90,
} OPTS;

struct async_resp_arg {
  httpd_handle_t hd;
  int fd;
  char *res;
  bool broadcast;
};

typedef struct {
  httpd_handle_t hd;
  int fd;
} ws_client_t;

int num_clients = 0;
ws_client_t *clients;

static SemaphoreHandle_t ws_mutex = NULL;
static atomic_uint pending_broadcasts = 0;
static atomic_size_t pending_bytes = 0;
static const unsigned MAX_PENDING_BROADCASTS = 4;

bool ws_has_clients(void) {
  if (!ws_mutex)
    return false;
  if (xSemaphoreTake(ws_mutex, 0) != pdTRUE)
    return false;
  bool connected = num_clients > 0;
  xSemaphoreGive(ws_mutex);
  return connected;
}

void ws_init(void) { ws_mutex = xSemaphoreCreateMutex(); }

static void add_client(const ws_client_t *new_client) {
  if (ws_mutex)
    xSemaphoreTake(ws_mutex, portMAX_DELAY);

  ws_client_t *new_clients =
      (ws_client_t *)realloc(clients, (num_clients + 1) * sizeof(ws_client_t));
  if (new_clients == NULL) {
    ESP_LOGE(TAG, "Adding client no mem");
    if (ws_mutex)
      xSemaphoreGive(ws_mutex);
    return;
  }

  clients = new_clients;
  clients[num_clients] = *new_client;
  num_clients++;

  if (ws_mutex)
    xSemaphoreGive(ws_mutex);
}

static void remove_client_by_fd(int fd) {
  if (ws_mutex)
    xSemaphoreTake(ws_mutex, portMAX_DELAY);

  int found_index = -1;
  for (int i = 0; i < num_clients; i++) {
    if (clients[i].fd == fd) {
      found_index = i;
      break;
    }
  }

  if (found_index == -1) {
    if (ws_mutex)
      xSemaphoreGive(ws_mutex);
    return;
  }

  for (int i = found_index; i < num_clients - 1; i++) {
    clients[i] = clients[i + 1];
  }

  num_clients--;

  if (num_clients > 0) {
    ws_client_t *new_clients = (ws_client_t *)realloc(clients, num_clients * sizeof(ws_client_t));
    if (new_clients != NULL) {
      clients = new_clients;
    }
  } else {
    free(clients);
    clients = NULL;
  }

  if (ws_mutex)
    xSemaphoreGive(ws_mutex);
}

void ws_on_close_handler(httpd_handle_t hd, int sockfd) {
  remove_client_by_fd(sockfd);
  close(sockfd);
}

static void ws_async_send(void *arg) {
  struct async_resp_arg *resp_arg = (struct async_resp_arg *)arg;
  httpd_handle_t hd = resp_arg->hd;
  int fd = resp_arg->fd;
  httpd_ws_frame_t ws_pkt;

  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  if (resp_arg->res != NULL) {
    ws_pkt.payload = (uint8_t *)resp_arg->res;
    ws_pkt.len = strlen(resp_arg->res);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    if (httpd_ws_get_fd_info(hd, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
      httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    }
    if (resp_arg->broadcast)
      pending_bytes -= ws_pkt.len + 1;
    free(resp_arg->res);
  }

  if (resp_arg->broadcast)
    --pending_broadcasts;
  free(resp_arg);
}

bool try_msg_clients(const char *msg, bool hook_request) {
  if (!msg)
    return false;
  const size_t length = strlen(msg);
  if (length > 16384 || !ws_mutex)
    return false;
  if (xSemaphoreTake(ws_mutex, 0) != pdTRUE)
    return false;
  bool queued = false;

  for (int i = 0; i < num_clients; i++) {
    // Reserve one slot for a hook when telemetry has filled its allowance.
    if (atomic_load(&pending_broadcasts) >= MAX_PENDING_BROADCASTS + (hook_request ? 1U : 0U) ||
        atomic_load(&pending_bytes) + length + 1 > 32768)
      break;
    ws_client_t cli = clients[i];
    struct async_resp_arg *resp_arg =
        (struct async_resp_arg *)malloc(sizeof(struct async_resp_arg));
    if (resp_arg == NULL) {
      ESP_LOGE(TAG, "Failed to allocate memory for async_resp_arg");
      continue;
    }

    resp_arg->hd = cli.hd;
    resp_arg->fd = cli.fd;
    resp_arg->res = (char *)malloc(length + 1);
    if (resp_arg->res == NULL) {
      ESP_LOGE(TAG, "Failed to allocate memory for response");
      free(resp_arg);
      continue;
    }
    memcpy(resp_arg->res, msg, length + 1);
    pending_bytes += length + 1;
    resp_arg->broadcast = true;
    ++pending_broadcasts;

    esp_err_t ret = httpd_queue_work(cli.hd, ws_async_send, resp_arg);
    if (ret != ESP_OK) {
      --pending_broadcasts;
      pending_bytes -= length + 1;
      ESP_LOGI(TAG, "Failed to queue work for async response");
      free(resp_arg->res);
      free(resp_arg);
    } else {
      queued = true;
    }
  }

  xSemaphoreGive(ws_mutex);
  return queued;
}

void msg_clients(const char *msg) { try_msg_clients(msg, false); }

esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    ESP_LOGI(TAG, "New Connection");

    ws_client_t cli = {.hd = req->handle, .fd = httpd_req_to_sockfd(req)};
    // Hook requests are small and latency-sensitive; do not coalesce TCP writes.
    const int no_delay = 1;
    if (setsockopt(cli.fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0)
      ESP_LOGW(TAG, "Could not enable TCP_NODELAY on WebSocket");
    add_client(&cli);

    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt;
  esp_err_t ret = ws_read_frame(req, &ws_pkt);
  if (ret != ESP_OK)
    return ret;
  uint8_t *buf = ws_pkt.payload;

  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
    struct async_resp_arg *resp_arg =
        (struct async_resp_arg *)malloc(sizeof(struct async_resp_arg));
    if (resp_arg == NULL) {
      ESP_LOGE(TAG, "Failed to allocate memory for async_resp_arg");
      free(buf);
      return ESP_ERR_NO_MEM;
    }
    resp_arg->res = NULL;
    resp_arg->broadcast = false;
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);

    cJSON *res = cJSON_CreateObject();
    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    if (root == NULL) {
      ESP_LOGE(TAG, "Failed to parse JSON");
      cJSON_AddBoolToObject(res, "success", false);
      cJSON_AddStringToObject(res, "error", "Invalid JSON format");
      resp_arg->res = cJSON_Print(res);
      free(buf);
      if (httpd_queue_work(req->handle, ws_async_send, resp_arg) != ESP_OK) {
        free(resp_arg->res);
        free(resp_arg);
      }
      cJSON_Delete(res);
      return ESP_OK;
    }

    cJSON *option = cJSON_GetObjectItem(root, "opt");
    if (cJSON_IsNumber(option)) {
      OPTS opt = (OPTS)option->valueint;
      switch (opt) {
      case WS_OPT_BOARD_STATUS: {
        const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
        if (cJSON_IsString(command) && !strcmp(command->valuestring, "status")) {
          resp_arg->res = gatt_state_json(gattAttackState);
          break;
        }
        cJSON_AddBoolToObject(res, "success", false);
        cJSON_AddStringToObject(res, "error", "Unknown board command");
        resp_arg->res = cJSON_Print(res);
        break;
      }
      case WS_OPT_SET_WIFI:
        cJSON_AddBoolToObject(res, "success", false);
        cJSON_AddStringToObject(res, "error", "Use Wi-Fi Settings");
        resp_arg->res = cJSON_Print(res);
        break;
      case WS_OPT_REBOOT:
        ws_reboot_response(root, res);
        resp_arg->res = cJSON_Print(res);
        break;
      case WS_OPT_GATTATTACK: {
        cJSON *item = cJSON_GetObjectItem(root, "action");
        if (cJSON_IsString(item) && !strcmp(item->valuestring, "status")) {
          resp_arg->res = gatt_state_json(gattAttackState);
        } else if (cJSON_IsString(item) && item->valuestring != NULL) {
          GattAttackWebEventParams ps;
          ps.id = -1;
          ps.request_id = 0;
          ps.mac = NULL;
          ps.hook_ret = NULL; // initialize other fields for safety
          ps.enable = false;

          ps.action = item->valuestring;

          cJSON *id = cJSON_GetObjectItem(root, "id");
          if (cJSON_IsNumber(id) && id != NULL) {
            ps.id = id->valueint;
          }

          cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
          if (cJSON_IsNumber(request_id) && request_id->valueint > 0) {
            ps.request_id = request_id->valueint;
          }

          cJSON *hook_ret = cJSON_GetObjectItem(root, "hook_ret");
          if (cJSON_IsString(hook_ret) && hook_ret->valuestring != NULL) {
            ps.hook_ret = hook_ret->valuestring;
          }

          cJSON *hook_enable = cJSON_GetObjectItem(root, "enable");
          if (cJSON_IsBool(hook_enable)) {
            ps.enable = cJSON_IsTrue(hook_enable);
          }

          cJSON *mac = cJSON_GetObjectItem(root, "mac");
          if (cJSON_IsString(mac) && mac->valuestring != NULL) {
            ps.mac = mac->valuestring;
          } else {
            ps.mac = NULL;
          }

          gatt_web_event(&ps);

          cJSON_Delete(root);
          cJSON_Delete(res);
          free(buf);
          free(resp_arg); // No response sent, so we must free this manually

          return ESP_OK;
        } else {
          cJSON_AddBoolToObject(res, "success", false);
          cJSON_AddStringToObject(res, "error", "Invalid Action");
          resp_arg->res = cJSON_Print(res);
        }
        break;
      }
      default:
        cJSON_AddBoolToObject(res, "success", false);
        cJSON_AddStringToObject(res, "error", "Invalid Option");
        resp_arg->res = cJSON_Print(res);
        break;
      }
    } else {
      cJSON_AddBoolToObject(res, "success", false);
      cJSON_AddStringToObject(res, "error", "Invalid Option");
      resp_arg->res = cJSON_Print(res);
    }

    cJSON_Delete(root);
    free(buf);

    if (resp_arg->res != NULL && httpd_queue_work(req->handle, ws_async_send, resp_arg) != ESP_OK) {
      free(resp_arg->res);
      free(resp_arg);
    } else if (resp_arg->res == NULL) {
      free(resp_arg);
    }

    cJSON_Delete(res);
    return ESP_OK;
  }

  ret = httpd_ws_send_frame(req, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
  }

  free(buf);
  return ret;
}
