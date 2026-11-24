#include "agent_api.h"
#include "ble/GattAttack.h"
#include "ble/GattAttackApp.h"
#include "ble/ble.h"
#include "capture/capture_store.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "network_service.h"
#include "uart/uart_bridge.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern bool connected;

typedef struct {
  uint32_t id;
  char kind;
  char mac[18];
  const char *state;
} Operation;

typedef struct {
  int64_t time_ms;
  char text[384];
  bool truncated;
} Preview;

typedef struct {
  SemaphoreHandle_t lock;
  uint32_t next_operation;
  uint32_t next_event;
  Operation operations[8];
  Preview events[24];
} Agent;

static Agent *agent;

static cJSON *failure(const char *message) {
  cJSON *result = cJSON_CreateObject();
  cJSON_AddStringToObject(result, "error", message);
  return result;
}

static const char *string(const cJSON *args, const char *key) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

static bool number(const cJSON *args, const char *key, uint32_t *out) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, key);
  if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
      value->valuedouble > UINT32_MAX || floor(value->valuedouble) != value->valuedouble)
    return false;
  *out = (uint32_t)value->valuedouble;
  return true;
}

static bool valid_mac(const char *mac) {
  if (strlen(mac) != 17) return false;
  for (unsigned i = 0; i < 17; i++) {
    if (i % 3 == 2 ? mac[i] != ':' : !strchr("0123456789abcdefABCDEF", mac[i]))
      return false;
  }
  return true;
}

static void update_operations(void) {
  char target[18];
  uart_bridge_target_mac(target, sizeof(target));
  for (unsigned i = 0; i < 8; i++) {
    Operation *op = &agent->operations[i];
    if (!op->id || strcmp(op->state, "running")) continue;
    if (op->kind == 's' && gattAttackState != GATT_ATTACK_STATE_SCANNING)
      op->state = gattAttackState == GATT_ATTACK_STATE_SCAN_RES ?
                  "completed" : "interrupted";
    if (op->kind == 'x' && gattAttackState == GATT_ATTACK_STATE_HOME)
      op->state = "completed";
    if (op->kind == 'c') {
      if (strcasecmp(op->mac, target)) op->state = "interrupted";
      else if (gattAttackState == GATT_ATTACK_STATE_RUNNING) op->state = "completed";
      else if (gattAttackState == GATT_ATTACK_STATE_HOME ||
               gattAttackState == GATT_ATTACK_STATE_CLIENT_CONNECT_FAIL ||
               gattAttackState == GATT_ATTACK_STATE_FAILED) op->state = "failed";
    }
  }
}

static cJSON *board_status(void) {
  cJSON *result = cJSON_CreateObject();
  char target[18];
  uart_bridge_target_mac(target, sizeof(target));
  cJSON_AddStringToObject(result, "firmware", "gattacker-mcp-1");
  cJSON_AddNumberToObject(result, "state", gattAttackState);
  cJSON_AddStringToObject(result, "target_mac", target);
  cJSON_AddBoolToObject(result, "target_connected", connected);
  cJSON_AddBoolToObject(result, "central_connected", uart_bridge_phone_connected());
  cJSON_AddBoolToObject(result, "browser_hooks", should_run_hooks());
  cJSON_AddNumberToObject(result, "uptime_ms", esp_timer_get_time() / 1000);
  cJSON_AddNumberToObject(result, "internal_free",
                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(result, "psram_free",
                          heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cJSON_AddItemToObject(result, "network", network_status());
  return result;
}

static cJSON *capture_status(void) {
  CaptureStoreStatus value = capture_store_status();
  const char *states[] = {"empty", "recording", "saving", "ready"};
  cJSON *result = cJSON_CreateObject();
  cJSON_AddStringToObject(result, "state", states[value.state]);
  cJSON_AddNumberToObject(result, "id", value.id);
  cJSON_AddNumberToObject(result, "packets", value.packets);
  cJSON_AddNumberToObject(result, "dropped", value.dropped);
  cJSON_AddNumberToObject(result, "bytes", value.bytes);
  cJSON_AddNumberToObject(result, "capacity", value.capacity);
  cJSON_AddBoolToObject(result, "available", value.available);
  cJSON_AddBoolToObject(result, "flipper_owner", value.flipper);
  cJSON_AddStringToObject(result, "reason", value.reason);
  return result;
}

static cJSON *session_control(const char *name, const cJSON *args) {
  bool scan = !strcmp(name, "scan_devices");
  bool stop = !strcmp(name, "session_stop");
  if (!stop && gattAttackState != GATT_ATTACK_STATE_HOME &&
      gattAttackState != GATT_ATTACK_STATE_SCAN_RES)
    return failure("Stop the active session first");
  if (stop && gattAttackState == GATT_ATTACK_STATE_SCANNING)
    return failure("Wait for scan completion");
  const char *mac = string(args, "mac");
  if (!scan && !stop && !valid_mac(mac)) return failure("A target MAC is required");
  GattAttackWebEventParams params = {.action = scan ? "scan" : stop ? "stop" : "start",
                                      .mac = mac};
  gatt_web_event(&params);
  if (scan && gattAttackState != GATT_ATTACK_STATE_SCANNING)
    return failure("Scan did not start");
  if (!scan && !stop && gattAttackState != GATT_ATTACK_STATE_START)
    return failure("Session did not start");
  uint32_t id = ++agent->next_operation;
  if (!id) id = ++agent->next_operation;
  Operation *op = &agent->operations[id % 8];
  *op = (Operation){.id = id, .kind = scan ? 's' : stop ? 'x' : 'c',
                     .state = "running"};
  snprintf(op->mac, sizeof(op->mac), "%s", mac);
  update_operations();
  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "operation_id", id);
  cJSON_AddStringToObject(result, "status", op->state);
  cJSON_AddBoolToObject(result, "accepted", true);
  return result;
}

static cJSON *traffic_read(const cJSON *args) {
  uint32_t after = 0, limit = 8;
  if ((cJSON_HasObjectItem(args, "after") && !number(args, "after", &after)) ||
      (cJSON_HasObjectItem(args, "limit") && !number(args, "limit", &limit)) ||
      !limit || limit > 16)
    return failure("after must be an unsigned cursor; limit must be 1-16");
  cJSON *result = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(result, "events");
  xSemaphoreTake(agent->lock, portMAX_DELAY);
  uint32_t newest = agent->next_event;
  uint32_t oldest = newest > 24 ? newest - 23 : 1;
  bool gap = after < oldest - 1 || after > newest;
  uint32_t start = gap ? oldest : after + 1;
  uint32_t cursor = after;
  for (uint32_t i = start; i <= newest && limit; i++, limit--) {
    const Preview *event = &agent->events[i % 24];
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "cursor", i);
    cJSON_AddNumberToObject(item, "time_ms", event->time_ms);
    cJSON_AddStringToObject(item, "record", event->text);
    cJSON_AddBoolToObject(item, "truncated", event->truncated);
    cJSON_AddItemToArray(list, item);
    cursor = i;
  }
  xSemaphoreGive(agent->lock);
  cJSON_AddBoolToObject(result, "gap", gap);
  cJSON_AddBoolToObject(result, "sampled", true);
  cJSON_AddNumberToObject(result, "next_cursor", cursor);
  return result;
}

static cJSON *scan_results(void) {
  if (gattAttackState == GATT_ATTACK_STATE_SCANNING) return failure("Scan is still running");
  cJSON *result = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(result, "devices");
  for (int i = 0; i < device_count; i++) {
    cJSON *item = cJSON_CreateObject();
    char mac[18];
    const uint8_t *value = devices[i].bd_addr;
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", value[0], value[1],
             value[2], value[3], value[4], value[5]);
    cJSON_AddStringToObject(item, "mac", mac);
    cJSON_AddStringToObject(item, "name", devices[i].name);
    cJSON_AddNumberToObject(item, "rssi", devices[i].rssi);
    cJSON_AddItemToArray(list, item);
  }
  return result;
}

static cJSON *services_list(void) {
  if (gattAttackState != GATT_ATTACK_STATE_RUNNING)
    return failure("No active services");
  cJSON *result = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(result, "services");
  unsigned count = 0;
  for (unsigned i = 0; i < clone.num_services && count < 64; i++, count++) {
    const service_t *service = &clone.services[i];
    char uuid[38];
    print_uuid(service->service_uuid, uuid);
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uuid", uuid);
    cJSON_AddNumberToObject(item, "handle", service->service_handle);
    cJSON *chars = cJSON_AddArrayToObject(item, "characteristics");
    cJSON_AddItemToArray(list, item);
    for (unsigned j = 0; j < service->num_chars && count < 64; j++, count++) {
      const char_t *characteristic = &service->chars[j];
      print_uuid(characteristic->char_uuid, uuid);
      cJSON *entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "uuid", uuid);
      cJSON_AddNumberToObject(entry, "handle", characteristic->char_handle);
      cJSON_AddNumberToObject(entry, "properties", characteristic->property);
      cJSON_AddItemToArray(chars, entry);
    }
  }
  return result;
}

static cJSON *capture_control(const char *name, const cJSON *args) {
  if (!strcmp(name, "capture_status")) return capture_status();
  uint32_t value;
  bool start = !strcmp(name, "capture_start");
  if (!number(args, start ? "seconds" : "id", &value) || !value)
    return failure(start ? "UTC Unix seconds required" : "Capture id required");
  const char *error = NULL;
  if (start) error = capture_store_start(value, false, false);
  else if (!strcmp(name, "capture_stop")) error = capture_store_stop(value);
  else if (!strcmp(name, "capture_clear")) error = capture_store_clear(value);
  else if (!strcmp(name, "capture_download")) {
    CaptureStoreStatus state = capture_store_status();
    if (state.id != value || state.state != CaptureReady)
      return failure("Save the capture first");
    char endpoint[64], url[128];
    network_endpoint(endpoint, sizeof(endpoint));
    size_t length = strlen(endpoint);
    if (length < 4) return failure("Network endpoint unavailable");
    endpoint[length - 4] = 0;
    snprintf(url, sizeof(url), "%s/api/capture/download?id=%lu", endpoint,
             (unsigned long)value);
    cJSON *result = capture_status();
    cJSON_AddStringToObject(result, "download_url", url);
    return result;
  } else return failure("Unknown capture operation");
  return error ? failure(error) : capture_status();
}

void agent_init(void) {
  if (agent) return;
#ifdef CONFIG_SPIRAM
  agent = heap_caps_calloc(1, sizeof(*agent), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!agent) agent = calloc(1, sizeof(*agent));
  if (agent) agent->lock = xSemaphoreCreateMutex();
}

cJSON *agent_call(const char *name, const cJSON *arguments) {
  if (!agent || !agent->lock) return failure("Agent unavailable");
  update_operations();
  if (!strcmp(name, "board_status")) return board_status();
  if (!strcmp(name, "network_status")) return network_status();
  if (!strcmp(name, "traffic_read")) return traffic_read(arguments);
  if (!strcmp(name, "scan_devices") || !strcmp(name, "session_start") ||
      !strcmp(name, "session_stop")) return session_control(name, arguments);
  if (!strcmp(name, "operation_status")) {
    uint32_t id;
    if (!number(arguments, "operation_id", &id) || !id ||
        agent->operations[id % 8].id != id)
      return failure("Unknown or expired operation; use board_status");
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", agent->operations[id % 8].state);
    cJSON_AddNumberToObject(result, "operation_id", id);
    return result;
  }
  if (!strcmp(name, "scan_results")) return scan_results();
  if (!strcmp(name, "services_list")) return services_list();
  if (!strncmp(name, "capture_", 8)) return capture_control(name, arguments);
  return failure("Unknown tool");
}

void agent_preview(const char *text) {
  if (!agent || strncmp(text, "traffic\t", 8) ||
      xSemaphoreTake(agent->lock, 0) != pdTRUE) return;
  uint32_t id = ++agent->next_event;
  Preview *event = &agent->events[id % 24];
  event->time_ms = esp_timer_get_time() / 1000;
  event->truncated = strlen(text) >= sizeof(event->text);
  snprintf(event->text, sizeof(event->text), "%s", text);
  xSemaphoreGive(agent->lock);
}
