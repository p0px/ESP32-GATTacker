#include "uart/uart_bridge.h"
#include "uart/uart_bridge_internal.h"
#include "ble/GattAttack.h"
#include "ble/GattAttackApp.h"
#include "ble/ble.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "network_pair.h"
#include "rules.h"
#include "capture/capture_wire.h"
#include "capture/capture_store.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern bool connected;
static atomic_bool phone_connected;
static char target[18];
static char selected[18];
static TrafficRule rules[RULE_COUNT];
static uint32_t revision = 1;
static SemaphoreHandle_t rule_mutex;
static atomic_uint rule_hits;

void uart_command_init(void) { rule_mutex = xSemaphoreCreateMutex(); }

bool uart_bridge_rules_active(void) {
  if (!rule_mutex) return false;
  xSemaphoreTake(rule_mutex, portMAX_DELAY);
  bool active = false;
  for (unsigned i = 0; i < RULE_COUNT; i++) active |= rules[i].enabled;
  xSemaphoreGive(rule_mutex);
  return active;
}

char *uart_bridge_hook(const char *type, const char *uuid, const char *hex) {
  if (!rule_mutex || !hex || !uart_bridge_rules_active()) return NULL;
  uint8_t data[517];
  size_t length;
  if (!rule_hex(hex, data, sizeof(data), &length)) return NULL;
  char operation = !strcmp(type, "write") ? 'W' :
                   !strcmp(type, "notify") ? 'N' : 'R';
  xSemaphoreTake(rule_mutex, portMAX_DELAY);
  bool changed = rule_apply(rules, operation, uuid, data, &length, sizeof(data));
  xSemaphoreGive(rule_mutex);
  if (!changed) return NULL;
  char *replacement = malloc(length * 2 + 1);
  if (!replacement) return NULL;
  rule_to_hex(data, length, replacement);
  atomic_fetch_add(&rule_hits, 1);
  return replacement;
}

static void reply(uint16_t id, const char *message) {
  (void)uart_emit_wait(WireReply, id, message, 250);
}

void uart_command_target(const char *mac) {
  snprintf(target, sizeof(target), "%s", mac ? mac : "");
  char event[32];
  snprintf(event, sizeof(event), "target\t%s", target);
  uart_bridge_event(event);
}

void uart_command_phone(bool active) {
  atomic_store(&phone_connected, active);
  uart_command_state();
}

void uart_bridge_target_mac(char *out, size_t size) {
  if (size) snprintf(out, size, "%s", target);
}

void uart_bridge_set_selected(const char *mac) {
  snprintf(selected, sizeof(selected), "%s", mac ? mac : "");
}

void uart_bridge_selected_mac(char *out, size_t size) {
  if (size) snprintf(out, size, "%s", selected);
}

bool uart_bridge_phone_connected(void) { return atomic_load(&phone_connected); }

void uart_command_state(void) {
  char event[240];
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  snprintf(event, sizeof(event),
           "status\t%d\t%d\t%d\t%d\t%lu\t%lu\thits:%lu ram:%u largest:%u uart_us:0",
           gattAttackState, connected, atomic_load(&phone_connected), should_run_hooks(),
           (unsigned long)revision, (unsigned long)uart_dropped(),
           (unsigned long)atomic_load(&rule_hits),
           (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps));
  uart_bridge_event(event);
  snprintf(event, sizeof(event), "target\t%s", target);
  uart_bridge_event(event);
}

static bool scan_results(void) {
  for (int i = 0; i < device_count; i++) {
    char event[160];
    char name[32];
    snprintf(name, sizeof(name), "%s", devices[i].name);
    for (char *p = name; *p; p++)
      if (*p < 32 || *p > 126) *p = '?';
    const uint8_t *mac = devices[i].bd_addr;
    snprintf(event, sizeof(event), "device\t%d\t%02X:%02X:%02X:%02X:%02X:%02X\t%d\t%s",
             i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], devices[i].rssi, name);
    if (!uart_emit_wait(WireEvent, 0, event, 250)) return false;
  }
  return true;
}

static bool send_rules(void) {
  if (!rule_mutex) return false;
  TrafficRule snapshot[RULE_COUNT];
  xSemaphoreTake(rule_mutex, portMAX_DELAY);
  memcpy(snapshot, rules, sizeof(snapshot));
  uint32_t current = revision;
  xSemaphoreGive(rule_mutex);
  for (unsigned i = 0; i < RULE_COUNT; i++) {
    char rule[160], event[200];
    rule_format(&snapshot[i], rule, sizeof(rule));
    snprintf(event, sizeof(event), "rule\t%u\t%lu\t%s", i, (unsigned long)current, rule);
    if (!uart_emit_wait(WireEvent, 0, event, 250)) return false;
  }
  return true;
}

static bool update_rule(char **fields, size_t count) {
  if (count != 9) return false;
  uint32_t expected, slot;
  TrafficRule replacement;
  if (!wire_number(fields[1], UINT32_MAX, &expected) || expected != revision ||
      !wire_number(fields[2], RULE_COUNT - 1, &slot) ||
      !rule_parse(&replacement, fields + 3, 6)) return false;
  GattAttackWebEventParams params = {.action = "enable_hooks", .enable = false};
  gatt_web_event(&params);
  xSemaphoreTake(rule_mutex, portMAX_DELAY);
  rules[slot] = replacement;
  revision++;
  xSemaphoreGive(rule_mutex);
  send_rules();
  return true;
}

static bool send_services(void) {
  unsigned count = 0;
  for (unsigned i = 0; i < clone.num_services && count < 64; i++) {
    char event[112], uuid[40];
    print_uuid(clone.services[i].service_uuid, uuid);
    snprintf(event, sizeof(event), "service\tS\t%u\t%s",
             clone.services[i].service_handle, uuid);
    if (!uart_emit_wait(WireEvent, 0, event, 250)) return false;
    count++;
    for (unsigned j = 0; j < clone.services[i].num_chars && count < 64; j++) {
      const char_t *characteristic = &clone.services[i].chars[j];
      print_uuid(characteristic->char_uuid, uuid);
      snprintf(event, sizeof(event), "service\tC:%02X\t%u\t%s",
               characteristic->property, characteristic->char_handle, uuid);
      if (!uart_emit_wait(WireEvent, 0, event, 250)) return false;
      count++;
    }
  }
  return true;
}

static void execute(uint16_t id, char **f, size_t count) {
  if (uart_network_command(id, f, count)) return;
  const char *result = "ok\taccepted";
  char response[96];
  GattAttackWebEventParams params = {.action = f[0], .id = -1};
  if (!strcmp(f[0], "hello")) {
    result = "ok\tGATTacker UART v1";
    bool capture = capture_store_ring() != NULL;
    uart_bridge_event(capture ? "capabilities\thci_pcapng" : "capabilities\t");
    uart_bridge_event(capture ? "capture_sink_support\t1" :
                    "capture_sink_support\t0");
    uart_bridge_event(capture ? "capture_ram_support\t1" :
                    "capture_ram_support\t0");
    if (!send_rules()) result = "error\trules unavailable; retry";
  } else if (!strcmp(f[0], "status")) {
    /* The common status snapshot follows each reply. */
  } else if (!strcmp(f[0], "results")) {
    if (gattAttackState == GATT_ATTACK_STATE_SCANNING)
      result = "error\tscan still running";
    else if (!scan_results()) result = "error\tdevice list interrupted; retry";
  } else if (!strcmp(f[0], "scan")) {
    if (gattAttackState != GATT_ATTACK_STATE_HOME &&
        gattAttackState != GATT_ATTACK_STATE_SCAN_RES)
      result = "error\tstop active session first";
    else {
      uart_bridge_set_selected("");
      gatt_web_event(&params);
      gatt_send_state(gattAttackState);
      if (gattAttackState != GATT_ATTACK_STATE_SCANNING)
        result = "error\tscan could not start; retry";
    }
  } else if (!strcmp(f[0], "select")) {
    if (count != 2 || (strcmp(f[1], "-") && !gatt_parse_mac(f[1], NULL)))
      result = "error\tinvalid selected address";
    else if (gattAttackState != GATT_ATTACK_STATE_HOME &&
             gattAttackState != GATT_ATTACK_STATE_SCAN_RES)
      result = "error\tstop the session before changing the target";
    else {
      uart_bridge_set_selected(strcmp(f[1], "-") ? f[1] : "");
      gatt_send_state(gattAttackState);
    }
  } else if (!strcmp(f[0], "start")) {
    if (count != 2 || !gatt_parse_mac(f[1], NULL))
      result = "error\tinvalid target address";
    else if (gattAttackState != GATT_ATTACK_STATE_HOME &&
             gattAttackState != GATT_ATTACK_STATE_SCAN_RES)
      result = "error\tboard busy";
    else {
      params.mac = f[1];
      uart_bridge_set_selected(f[1]);
      uart_command_target(f[1]);
      gatt_web_event(&params);
    }
  } else if (!strcmp(f[0], "stop")) {
    if (gattAttackState == GATT_ATTACK_STATE_SCANNING)
      result = "error\ttransition in progress; retry stop";
    else if (gattAttackState != GATT_ATTACK_STATE_HOME) {
      gatt_web_event(&params);
      uart_command_phone(false);
      uart_command_target("");
    }
  } else if (!strcmp(f[0], "services")) {
    if (gattAttackState != GATT_ATTACK_STATE_RUNNING)
      result = "error\tno active services";
    else if (!send_services()) result = "error\tservice list interrupted; retry";
  } else if (!strcmp(f[0], "rules")) {
    if (!send_rules()) result = "error\trules unavailable; retry";
  } else if (!strcmp(f[0], "rule_set")) {
    if (!update_rule(f, count)) result = "error\trule invalid or revision stale; reload";
  } else if (!strcmp(f[0], "browser_on") || !strcmp(f[0], "browser_off")) {
    params.action = "enable_hooks";
    params.enable = !strcmp(f[0], "browser_on");
    if (params.enable && uart_bridge_rules_active())
      result = "error\tdisable onboard rules first";
    else gatt_web_event(&params);
  } else if (!strcmp(f[0], "pair_allow") || !strcmp(f[0], "pair_deny")) {
    uint32_t pending;
    if (count != 2 || !wire_number(f[1], UINT32_MAX, &pending) || !pending ||
        !network_pair_decide(pending, !strcmp(f[0], "pair_allow")))
      result = "error\tPair request expired";
    else result = !strcmp(f[0], "pair_allow") ? "ok\tapproved" : "ok\tdenied";
  } else {
    result = "error\tunknown command";
  }
  snprintf(response, sizeof(response), "%s", result);
  reply(id, response);
  uart_command_state();
  capture_wire_status();
}

void uart_command_execute(uint16_t id, const char *text) {
  char record[WIRE_MAX];
  snprintf(record, sizeof(record), "%s", text);
  char *fields[12];
  size_t count = wire_fields(record, fields, 12);
  if (count && capture_wire_receive(fields, count)) return;
  if (count && capture_wire_command(id, fields, count)) {
    uart_command_state();
    return;
  }
  if (count) execute(id, fields, count);
  else reply(id, "error\ttoo many fields");
}
