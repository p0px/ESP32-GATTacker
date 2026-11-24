#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int esp_err_t;
typedef void *httpd_handle_t;
typedef void *SemaphoreHandle_t;
enum { ESP_OK = 0, pdTRUE = 1, portMAX_DELAY = 100, HTTPD_WS_TYPE_TEXT = 1,
       HTTPD_WS_CLIENT_WEBSOCKET = 2 };
typedef struct { uint8_t *payload; size_t len; int type; } httpd_ws_frame_t;
static int locked, queued, delivered, waited;
static bool fail_queue, client_open = true;
static bool unlock_on_wait;
static void (*work[8])(void *);
static void *args[8];
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, int timeout) {
  assert(mutex);
  if (locked && timeout && unlock_on_wait) {
    waited = timeout;
    locked = 0;
  }
  if (locked) return 0;
  locked = 1;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex && locked); locked = 0; }
static int httpd_ws_get_fd_info(httpd_handle_t hd, int fd) {
  assert(hd && fd == 3);
  return client_open ? HTTPD_WS_CLIENT_WEBSOCKET : 0;
}
static int httpd_ws_send_frame_async(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame) {
  assert(hd && fd == 3 && frame->type == HTTPD_WS_TYPE_TEXT);
  assert(frame->len == strlen((const char *)frame->payload));
  delivered++;
  return ESP_OK;
}
static esp_err_t httpd_queue_work(httpd_handle_t hd, void (*fn)(void *), void *arg) {
  assert(hd);
  if (fail_queue) return -1;
  assert(queued < 8);
  work[queued] = fn;
  args[queued++] = arg;
  return ESP_OK;
}
#include "ws_queue_production.inc"

static void drain(void) {
  for (int i = 0; i < queued; i++) work[i](args[i]);
  queued = 0;
  assert(atomic_load(&pending_broadcasts) == 0 && atomic_load(&pending_bytes) == 0);
}
int main(void) {
  ws_init();
  assert(!ws_has_clients() && !try_msg_clients("message", false));
  const ws_client_t client = {(void *)1, 3};
  add_client(&client);
  assert(ws_has_clients());
  for (int i = 0; i < 4; i++) assert(try_msg_clients("message", false));
  assert(!try_msg_clients("message", false));
  assert(try_msg_clients("request", true));
  assert(!try_msg_clients("request", true));
  drain();
  assert(delivered == 5);

  fail_queue = true;
  assert(!try_msg_clients("failed", false));
  assert(atomic_load(&pending_broadcasts) == 0 && atomic_load(&pending_bytes) == 0);
  fail_queue = false;
  locked = 1;
  assert(!ws_has_clients() && !try_msg_clients("busy", false));
  unlock_on_wait = true;
  assert(try_msg_clients("request", true) && waited == 5);
  drain();
  unlock_on_wait = false;
  locked = 0;

  char first[16301], second[14501];
  memset(first, 'a', sizeof(first));
  memset(second, 'b', sizeof(second));
  first[16300] = 0;
  second[14500] = 0;
  assert(try_msg_clients(first, false));
  assert(!try_msg_clients(second, false));
  assert(try_msg_clients("reserved hook", true));
  drain();

  char large[16386];
  memset(large, 'x', sizeof(large));
  large[16384] = 0;
  assert(try_msg_clients(large, false));
  assert(!try_msg_clients(large, false));
  client_open = false;
  drain();
  assert(delivered == 8);
  large[16384] = 'x';
  large[16385] = 0;
  assert(!try_msg_clients(large, false) && !try_msg_clients(NULL, false));
  remove_client_by_fd(3);
  assert(!ws_has_clients() && num_clients == 0 && !clients);
}
