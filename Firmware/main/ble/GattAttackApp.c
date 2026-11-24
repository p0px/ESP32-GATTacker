#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "ble/GattAttackApp.h"
#include "ble/GattAttack.h"
#include "ble/ble.h"
#include "wifi/ws.h"
#include "cJSON.h"
#include "uart/uart_bridge.h"

#define TAG "GattAttackApp"

extern bool connected;
static void scan_devices(void);
static SemaphoreHandle_t session_mutex;

static atomic_bool hooks_enabled = false;
static atomic_int hook_fault = 0; // 1 = deadline, 2 = transport queue
static portMUX_TYPE hook_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t next_hook_id = 0;
static uint32_t pending_hook_id = 0;
static char *hook_reply = NULL;
// Created on the Bluetooth callback task and retained for late replies.
static SemaphoreHandle_t hook_signal = NULL;
// Browser scheduling and Wi-Fi round trips need more than a single BLE interval.
static const uint32_t HOOK_TIMEOUT_MS = 250;
static const unsigned HOOK_TIMEOUT_LIMIT = 3;
static unsigned hook_timeout_streak = 0; // Protected by hook_mux.
static unsigned hook_queue_failure_streak = 0;
static atomic_uint hook_last_ms = 0;

GattAttackState gattAttackState = GATT_ATTACK_STATE_HOME;

static int hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool gatt_parse_mac(const char *text, uint8_t address[6]) {
  if (!text || strlen(text) != 17) return false;
  for (unsigned i = 0; i < 6; i++) {
    int high = hex_digit(text[i * 3]);
    int low = hex_digit(text[i * 3 + 1]);
    if (high < 0 || low < 0 || (i < 5 && text[i * 3 + 2] != ':')) return false;
    if (address) address[i] = (uint8_t)((high << 4) | low);
  }
  return true;
}

static void send_error(const char *message) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply) return;
  cJSON_AddBoolToObject(reply, "success", false);
  cJSON_AddStringToObject(reply, "error", message);
  char *json = cJSON_PrintUnformatted(reply);
  if (json) msg_clients(json);
  free(json);
  cJSON_Delete(reply);
}

void gatt_app_init(void) {
  if (!session_mutex) session_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(session_mutex ? ESP_OK : ESP_ERR_NO_MEM);
}

bool should_run_hooks(void) { return ws_has_clients() && atomic_load(&hooks_enabled); }

char *run_hook(const char *type, const char *uuid, const char *value) {
  if (!value || !atomic_load(&hooks_enabled))
    return NULL;
  if (!hook_signal)
    hook_signal = xSemaphoreCreateBinary();
  if (!hook_signal)
    return NULL;
  portENTER_CRITICAL(&hook_mux);
  if (pending_hook_id) {
    portEXIT_CRITICAL(&hook_mux);
    return NULL; // Caller keeps its original payload.
  }
  // A late signal from a timed-out request must not satisfy this request.
  xSemaphoreTake(hook_signal, 0);
  if (++next_hook_id > INT32_MAX)
    next_hook_id = 1;
  const uint32_t id = next_hook_id;
  pending_hook_id = id;
  portEXIT_CRITICAL(&hook_mux);

  cJSON *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(res, strcmp(type, "write") == 0 ? "onWrite" : "onRead", true);
  cJSON_AddNumberToObject(res, "request_id", id);
  cJSON_AddStringToObject(res, "uuid", uuid);
  cJSON_AddStringToObject(res, "value", value);
  char *json = cJSON_PrintUnformatted(res);
  cJSON_Delete(res);
  const bool sent = json && try_msg_clients(json, true);
  free(json);

  const TickType_t started = xTaskGetTickCount();
  const TickType_t budget = pdMS_TO_TICKS(HOOK_TIMEOUT_MS);
  for (;;) {
    portENTER_CRITICAL(&hook_mux);
    const TickType_t elapsed = xTaskGetTickCount() - started;
    const bool cancelled = !atomic_load(&hooks_enabled);
    if (hook_reply || !sent || cancelled || elapsed >= budget) {
      char *reply = hook_reply;
      hook_reply = NULL;
      pending_hook_id = 0;
      const bool failed = !reply && !cancelled;
      hook_last_ms = elapsed * portTICK_PERIOD_MS;
      if (reply) {
        hook_timeout_streak = 0;
        hook_queue_failure_streak = 0;
      } else if (failed && sent) {
        ++hook_timeout_streak;
        hook_queue_failure_streak = 0;
      } else if (failed) {
        ++hook_queue_failure_streak;
        hook_timeout_streak = 0;
      }
      const unsigned misses = sent ? hook_timeout_streak : hook_queue_failure_streak;
      const bool disabled = failed && misses >= HOOK_TIMEOUT_LIMIT;
      if (disabled) {
        hooks_enabled = false;
        hook_fault = sent ? 1 : 2;
      }
      portEXIT_CRITICAL(&hook_mux);
      if (failed) {
        char message[160];
        if (sent) {
          snprintf(message, sizeof(message),
                   "%s: hook reply exceeded %lu ms (%u/%u); original payload forwarded",
                   disabled ? "Hooks disabled" : "Hook timeout",
                   (unsigned long)HOOK_TIMEOUT_MS, misses, HOOK_TIMEOUT_LIMIT);
        } else {
          snprintf(message, sizeof(message),
                   "%s: request could not be queued (%u/%u); original payload forwarded",
                   disabled ? "Hooks disabled" : "Hook queue busy", misses, HOOK_TIMEOUT_LIMIT);
        }
        gatt_send_output(message, false);
        gatt_send_state(gattAttackState);
      }
      return reply;
    }
    portEXIT_CRITICAL(&hook_mux);
    // The HTTP reply wakes us immediately; no polling interval is added.
    xSemaphoreTake(hook_signal, budget - elapsed);
  }
}

void gatt_send_output(const char *output, bool isJSON) {
  if (isJSON) {
    msg_clients(output);
    return;
  }

  cJSON *res = cJSON_CreateObject();
  cJSON_AddStringToObject(res, "msg", output);
  char *json = cJSON_Print(res);

  ESP_LOGI(TAG, "%s", output);

  msg_clients(json);
  free(json);
  cJSON_Delete(res);
};

static void scan_task(void *params) {
  scan_devices();
  vTaskDelete(NULL);
};

char *gatt_state_json(int state) {
  cJSON *res = cJSON_CreateObject();
  if (!res) return NULL;
  cJSON_AddNumberToObject(res, "state", state);
  char selected[18];
  char target[18];
  uart_bridge_selected_mac(selected, sizeof(selected));
  uart_bridge_target_mac(target, sizeof(target));
  cJSON_AddStringToObject(res, "selected_mac", selected);
  cJSON_AddStringToObject(res, "target_mac", target);
  cJSON_AddBoolToObject(res, "target_connected", connected);
  cJSON_AddBoolToObject(res, "central_connected", uart_bridge_phone_connected());
  cJSON_AddNumberToObject(res, "free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(res, "min_free_heap", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(res, "uptime_ms", esp_timer_get_time() / 1000);
  cJSON_AddBoolToObject(res, "hooks_enabled", atomic_load(&hooks_enabled));
  cJSON_AddNumberToObject(res, "hook_last_ms", atomic_load(&hook_last_ms));
  cJSON_AddNumberToObject(res, "hook_timeout_ms", HOOK_TIMEOUT_MS);
  const int fault = atomic_load(&hook_fault);
  cJSON_AddStringToObject(res, "hook_error",
                          fault == 1 ? "Hooks disabled after 3 consecutive hook reply timeouts "
                                       "(250 ms each); original traffic passes through. "
                                       "Re-enable to retry."
                          : fault == 2 ? "Hooks disabled after 3 consecutive request queue "
                                         "failures; original traffic passes through. "
                                         "Re-enable hooks to retry."
                                       : "");
  char *json = cJSON_PrintUnformatted(res);
  cJSON_Delete(res);
  return json;
}

void gatt_send_state(int state) {
  char *json = gatt_state_json(state);
  if (json) msg_clients(json);
  uart_bridge_state();
  free(json);
};

static void scan_devices(void) {
  gatt_send_state((int)gattAttackState);

  // scan for devices
  if (!ble_stack_init()) {
    gattAttackState = GATT_ATTACK_STATE_FAILED;
    gatt_send_output("Bluetooth initialization failed", false);
    gatt_send_state(gattAttackState);
    return;
  }

  start_ble_scan(10, NULL);

  ble_stack_deinit(false);

  ESP_LOGI(TAG, "Scan Finished. Got %i Devices", device_count);

  cJSON *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(res, "scan_done", true);
  char *json = cJSON_Print(res);

  msg_clients(json);

  free(json);
  cJSON_Delete(res);

  gattAttackState = GATT_ATTACK_STATE_SCAN_RES;
  gatt_send_state((int)gattAttackState);
};

static void stop_session(void) {
  xSemaphoreTake(session_mutex, portMAX_DELAY);
  if (gattAttackState == GATT_ATTACK_STATE_SCANNING ||
      gattAttackState == GATT_ATTACK_STATE_HOME) {
    xSemaphoreGive(session_mutex);
    return;
  }
  stop_gatt_attack();
  gattAttackState = GATT_ATTACK_STATE_HOME;
  uart_bridge_target("");
  uart_bridge_phone(false);
  gatt_send_state((int)gattAttackState);
  xSemaphoreGive(session_mutex);
};

static void start_session(esp_bd_addr_t target) {
  xSemaphoreTake(session_mutex, portMAX_DELAY);
  if (gattAttackState != GATT_ATTACK_STATE_HOME &&
      gattAttackState != GATT_ATTACK_STATE_SCAN_RES) {
    xSemaphoreGive(session_mutex);
    return;
  }
  char target_mac[18];
  snprintf(target_mac, sizeof(target_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           target[0], target[1], target[2], target[3], target[4], target[5]);
  uart_bridge_target(target_mac);
  uart_bridge_set_selected(target_mac);
  gattAttackState = GATT_ATTACK_STATE_START;
  gatt_send_state((int)gattAttackState);
  start_gatt_attack(target);
  xSemaphoreGive(session_mutex);
};

void gatt_web_event(GattAttackWebEventParams *ps) {
  if (strcmp(ps->action, "status") == 0) {
    gatt_send_state((int)gattAttackState);
    return;
  }

  if (strcmp(ps->action, "start") == 0) {
    if (ps->mac != NULL) {
      esp_bd_addr_t target;
      if (!gatt_parse_mac(ps->mac, target)) send_error("Invalid target address");
      else start_session(target);
      return;
    }

    if (ps->id < 0 || ps->id >= device_count) {
      send_error("Invalid device ID");
      return;
    }

    start_session(devices[ps->id].bd_addr);
    return;
  }

  if (strcmp(ps->action, "select") == 0) {
    if (gattAttackState != GATT_ATTACK_STATE_HOME &&
        gattAttackState != GATT_ATTACK_STATE_SCAN_RES) return;
    const char *mac = ps->mac;
    if (!mac || !strcmp(mac, "-")) uart_bridge_set_selected("");
    else if (gatt_parse_mac(mac, NULL)) uart_bridge_set_selected(mac);
    else send_error("Invalid selected address");
    gatt_send_state(gattAttackState);
    return;
  }

  if (strcmp(ps->action, "stop") == 0) {
    stop_session();
  }

  if (strcmp(ps->action, "scan") == 0) {
    xSemaphoreTake(session_mutex, portMAX_DELAY);
    if (gattAttackState != GATT_ATTACK_STATE_HOME &&
        gattAttackState != GATT_ATTACK_STATE_SCAN_RES) {
      xSemaphoreGive(session_mutex);
      return;
    }
    gattAttackState = GATT_ATTACK_STATE_SCANNING;
    if (xTaskCreate(scan_task, "gatk", 6000, NULL, 0, NULL) != pdPASS) {
      gattAttackState = GATT_ATTACK_STATE_HOME;
      gatt_send_state(gattAttackState);
    }
    xSemaphoreGive(session_mutex);
    return;
  }

  if (strcmp(ps->action, "results") == 0) {
    // Create the root JSON object
    cJSON *root = cJSON_CreateObject();
    cJSON *results_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "results", results_array);

    for (int i = 0; i < device_count; i++) {
      // Create a JSON object for each device
      cJSON *device_obj = cJSON_CreateObject();
      char mac_str[18];
      snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", devices[i].bd_addr[0],
               devices[i].bd_addr[1], devices[i].bd_addr[2], devices[i].bd_addr[3],
               devices[i].bd_addr[4], devices[i].bd_addr[5]);

      // Add properties to the device object
      cJSON_AddNumberToObject(device_obj, "id", i);
      cJSON_AddStringToObject(device_obj, "mac", mac_str);
      cJSON_AddStringToObject(device_obj, "name", devices[i].name);
      cJSON_AddNumberToObject(device_obj, "rssi", devices[i].rssi);

      // Add the device object to the array
      cJSON_AddItemToArray(results_array, device_obj);
    }

    // Convert the root object to a formatted JSON string
    char *json_string = cJSON_Print(root);

    // Send json and free
    msg_clients(json_string);
    free(json_string);
    cJSON_Delete(root);
    return;
  }

  if (strcmp(ps->action, "hookret") == 0) {
    if (!ps->hook_ret || !ps->request_id)
      return;
    char *reply = strdup(ps->hook_ret);
    if (!reply)
      return;
    bool accepted = false;
    portENTER_CRITICAL(&hook_mux);
    if (pending_hook_id == ps->request_id && !hook_reply) {
      hook_reply = reply;
      reply = NULL;
      accepted = true;
    }
    portEXIT_CRITICAL(&hook_mux);
    if (accepted)
      xSemaphoreGive(hook_signal);
    free(reply);
    return;
  }

  if (strcmp(ps->action, "enable_hooks") == 0) {
    // Wake an in-flight request when the user disables hooks.
    portENTER_CRITICAL(&hook_mux);
    hook_fault = 0;
    hook_timeout_streak = 0;
    hook_queue_failure_streak = 0;
    hooks_enabled = ps->enable;
    SemaphoreHandle_t signal = pending_hook_id ? hook_signal : NULL;
    portEXIT_CRITICAL(&hook_mux);
    if (!ps->enable && signal)
      xSemaphoreGive(signal);
    char out[32];
    snprintf(out, sizeof(out), "hooks %s", hooks_enabled ? "enabled" : "disabled");
    gatt_send_output(out, false);
    gatt_send_state(gattAttackState);
    return;
  }
};
