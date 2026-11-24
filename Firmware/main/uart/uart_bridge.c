#include "uart/uart_bridge.h"
#include "uart/uart_bridge_internal.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_pair.h"
#include "agent_api.h"
#include "capture/capture_wire.h"
#include "wifi/ws.h"
#include "cJSON.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_GATTACKER_UART_TX
#define CONFIG_GATTACKER_UART_TX 43
#define CONFIG_GATTACKER_UART_RX 44
#define CONFIG_GATTACKER_UART_BAUD 115200
#endif

typedef struct {
  WireKind kind;
  uint16_t id;
  char text[WIRE_MAX];
} UartPacket;

static QueueHandle_t incoming;
static QueueHandle_t outgoing;
static WireParser parser;
static atomic_uint dropped;
static atomic_llong last_received_us;
static atomic_llong next_traffic_us;

uint32_t uart_dropped(void) { return atomic_load(&dropped); }

bool uart_emit_wait(WireKind kind, uint16_t id, const char *text, uint32_t wait_ms) {
  if (!outgoing || !text || strlen(text) >= WIRE_MAX) return false;
  UartPacket *packet = heap_caps_malloc(sizeof(*packet), MALLOC_CAP_8BIT);
  if (!packet) {
    atomic_fetch_add(&dropped, 1);
    return false;
  }
  packet->kind = kind;
  packet->id = id;
  snprintf(packet->text, sizeof(packet->text), "%s", text);
  bool sent = xQueueSend(outgoing, packet, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
  if (!sent) atomic_fetch_add(&dropped, 1);
  heap_caps_free(packet);
  return sent;
}

void uart_emit(WireKind kind, uint16_t id, const char *text) {
  (void)uart_emit_wait(kind, id, text, 0);
}

void uart_bridge_event(const char *event) { uart_emit(WireEvent, 0, event); }

void uart_bridge_traffic(const char *operation, const char *direction,
                         const char *uuid, uint16_t handle, const char *hex) {
  int64_t now = esp_timer_get_time();
  int64_t expected = atomic_load(&next_traffic_us);
  if (now < expected || !atomic_compare_exchange_strong(&next_traffic_us, &expected,
                                                          now + 100000)) return;
  char *event = heap_caps_malloc(WIRE_MAX, MALLOC_CAP_8BIT);
  if (!event) {
    atomic_fetch_add(&dropped, 1);
    return;
  }
  char op = strstr(operation, "WRITE") ? 'W' :
            strstr(operation, "NOTIFY") || strstr(operation, "INDICATE") ? 'N' : 'R';
  const char *data = hex ? hex : "";
  snprintf(event, WIRE_MAX, "traffic\t%c\t%s\t%s\t%u\t%.1034s\t%s", op,
           direction, uuid, handle, data, strlen(data) > 1034 ? "truncated" : operation);
  agent_preview(event);
  if (uart_bridge_connected() || ws_has_clients()) uart_bridge_event(event);
  heap_caps_free(event);
}

static void receive(void *context, WireKind kind, uint16_t id, const char *text) {
  (void)context;
  if (kind != WireCommand) return;
  atomic_store(&last_received_us, esp_timer_get_time());
  UartPacket packet = {.kind = kind, .id = id};
  snprintf(packet.text, sizeof(packet.text), "%s", text);
  if (xQueueSend(incoming, &packet, 0) != pdTRUE)
    uart_emit(WireReply, id, "error\tbusy; retry command");
}

static void receive_task(void *context) {
  (void)context;
  uint8_t bytes[256];
  for (;;) {
    int count = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), pdMS_TO_TICKS(50));
    for (int i = 0; i < count; i++) wire_feed(&parser, bytes[i], receive, NULL);
  }
}

static void command_task(void *context) {
  (void)context;
  UartPacket packet;
  for (;;) {
    if (xQueueReceive(incoming, &packet, portMAX_DELAY) == pdTRUE)
      uart_command_execute(packet.id, packet.text);
  }
}

static void transmit_task(void *context) {
  (void)context;
  UartPacket packet;
  uint8_t frame[WIRE_FRAME_MAX];
  for (;;) {
    bool capture = false;
    if (xQueueReceive(outgoing, &packet, 0) != pdTRUE) {
      if (capture_wire_poll(packet.text, sizeof(packet.text))) {
        packet.kind = WireEvent;
        packet.id = 0;
        capture = true;
      } else if (xQueueReceive(outgoing, &packet, pdMS_TO_TICKS(10)) != pdTRUE)
        continue;
    }
    size_t size = wire_encode(packet.kind, packet.id, packet.text, frame, sizeof(frame));
    if (size) uart_write_bytes(UART_NUM_1, frame, size);
    if (capture || !ws_has_clients() || !strncmp(packet.text, "traffic\t", 8) ||
        !strncmp(packet.text, "wifi_key\t", 9) ||
        !strncmp(packet.text, "pair_request\t", 13) ||
        !strncmp(packet.text, "capture_export_", 15)) continue;
    cJSON *json = cJSON_CreateObject();
    if (!json) continue;
    cJSON_AddStringToObject(json, "gpio", packet.text);
    cJSON_AddNumberToObject(json, "request_id", packet.id);
    char *body = cJSON_PrintUnformatted(json);
    if (body) {
      msg_clients(body);
      free(body);
    }
    cJSON_Delete(json);
  }
}

bool uart_bridge_connected(void) {
  int64_t last = atomic_load(&last_received_us);
  return last && esp_timer_get_time() - last < 10000000;
}

static bool pair_connected(void *context) {
  (void)context;
  return uart_bridge_connected();
}

static void pair_request(void *context, uint32_t id, const char *peer) {
  (void)context;
  char event[64];
  snprintf(event, sizeof(event), "pair_request\t%lu\t%s", (unsigned long)id, peer);
  uart_bridge_event(event);
}

void uart_bridge_start(void) {
  if (incoming) return;
#ifdef CONFIG_SPIRAM
  const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
  incoming = xQueueCreateWithCaps(4, sizeof(UartPacket), caps);
  outgoing = xQueueCreateWithCaps(8, sizeof(UartPacket), caps);
  ESP_ERROR_CHECK(incoming && outgoing ? ESP_OK : ESP_ERR_NO_MEM);
  uart_config_t config = {
    .baud_rate = CONFIG_GATTACKER_UART_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, CONFIG_GATTACKER_UART_TX,
                               CONFIG_GATTACKER_UART_RX, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 4096, 4096, 0, NULL, 0));
  network_pair_bind(pair_connected, pair_request, NULL);
  uart_command_init();
  ESP_ERROR_CHECK(xTaskCreate(receive_task, "gpio_rx", 4096, NULL, 4, NULL) == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(command_task, "gpio_command", 8192, NULL, 3, NULL) == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(transmit_task, "gpio_tx", 8192, NULL, 2, NULL) == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);
}

void uart_bridge_state(void) { uart_command_state(); }
void uart_bridge_target(const char *mac) { uart_command_target(mac); }
void uart_bridge_phone(bool connected) { uart_command_phone(connected); }
