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

#define TAG "GattAttackApp"

typedef struct {
  esp_bd_addr_t target;
} GattAttackParams;
static void scan_devices(void);
static void session_task(void *taskParams);

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
static atomic_uint hook_last_ms = 0;

TaskHandle_t gattTaskHandle = NULL;
GattAttackState gattAttackState = GATT_ATTACK_STATE_HOME;

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
      if (reply)
        hook_timeout_streak = 0;
      if (failed && sent)
        ++hook_timeout_streak;
      const bool disabled = failed && (!sent || hook_timeout_streak >= HOOK_TIMEOUT_LIMIT);
      if (disabled) {
        hooks_enabled = false;
        hook_fault = sent ? 1 : 2;
      }
      const unsigned misses = hook_timeout_streak;
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
                   "Hooks disabled: request could not be queued; original payload forwarded");
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

void gatt_send_state(int state) {
  cJSON *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(res, "state", state);
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
                          : fault == 2 ? "Hooks disabled: request queue unavailable; original "
                                         "traffic passes through. Re-enable hooks to retry."
                                       : "");
  char *json = cJSON_Print(res);

  msg_clients(json);

  free(json);
  cJSON_Delete(res);
};

static void scan_devices(void) {
  gatt_send_state((int)gattAttackState);

  // scan for devices
  ble_stack_init();

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
  if (gattAttackState == GATT_ATTACK_STATE_SCANNING)
    return;
  stop_gatt_attack();

  if (gattTaskHandle != NULL) {
    vTaskDelete(gattTaskHandle);
    gattTaskHandle = NULL;
  }

  gattAttackState = GATT_ATTACK_STATE_HOME;
  gatt_send_state((int)gattAttackState);
};

static void start_session(esp_bd_addr_t target) {
  if (gattTaskHandle || gattAttackState == GATT_ATTACK_STATE_SCANNING)
    return;
  GattAttackParams *params = calloc(1, sizeof(GattAttackParams));
  if (!params)
    return;
  memcpy(params->target, target, sizeof(esp_bd_addr_t));

  BaseType_t ret = xTaskCreate(&session_task, "gatk", 20000, params, 0, &gattTaskHandle);
  if (ret != pdPASS) {
    free(params);
    gattTaskHandle = NULL;
    ESP_LOGE(TAG, "Cannot allocate attack task");
    return;
  }
  gattAttackState = GATT_ATTACK_STATE_START;
  gatt_send_state((int)gattAttackState);
};

static void session_task(void *taskParams) {
  GattAttackParams *params = taskParams;
  start_gatt_attack(params->target);
  free(params);
  while (1) {
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
};

void gatt_web_event(GattAttackWebEventParams *ps) {
  if (strcmp(ps->action, "status") == 0) {
    gatt_send_state((int)gattAttackState);
    return;
  }

  if (strcmp(ps->action, "start") == 0) {
    if (ps->mac != NULL) {
      esp_bd_addr_t target;
      unsigned values[6];
      if (sscanf(ps->mac, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3],
                 &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
          target[i] = (uint8_t)values[i];
        }
        start_session(target);
        return;
      }
    }

    // Validate ID sent
    if (ps->id < 0 || ps->id >= device_count) {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddBoolToObject(root, "success", false);
      cJSON_AddStringToObject(root, "error", "Invalid Device ID");

      // Convert the root object to a formatted JSON string
      char *json_string = cJSON_Print(root);

      // Send json and free
      if (json_string) {
        msg_clients(json_string);
        free(json_string);
      }
      cJSON_Delete(root);
      return;
    }

    start_session(devices[ps->id].bd_addr);
    return;
  }

  if (strcmp(ps->action, "stop") == 0) {
    stop_session();
  }

  if (strcmp(ps->action, "scan") == 0) {
    if (gattTaskHandle || gattAttackState == GATT_ATTACK_STATE_SCANNING)
      return;
    gattAttackState = GATT_ATTACK_STATE_SCANNING;
    if (xTaskCreate(scan_task, "gatk", 6000, NULL, 0, NULL) != pdPASS) {
      gattAttackState = GATT_ATTACK_STATE_HOME;
      gatt_send_state(gattAttackState);
    }
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
