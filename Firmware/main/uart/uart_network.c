#include "uart/uart_bridge_internal.h"
#include "cJSON.h"
#include "network_service.h"
#include "rules.h"
#include <stdio.h>
#include <string.h>

static const char *json_text(const cJSON *json, const char *key) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

static void snapshot(void) {
  cJSON *json = network_status();
  if (!json) return;
  char ap[65], station[65], event[300];
  const char *ap_name = json_text(json, "ap_ssid");
  const char *station_name = json_text(json, "station_ssid");
  rule_to_hex((const uint8_t *)ap_name, strlen(ap_name), ap);
  rule_to_hex((const uint8_t *)station_name, strlen(station_name), station);
  const cJSON *reason = cJSON_GetObjectItemCaseSensitive(json, "disconnect_reason");
  snprintf(event, sizeof(event), "wifi\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%s",
           json_text(json, "mode"), ap, station, json_text(json, "state"),
           json_text(json, "station_ip"),
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ap_active")),
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "mcp_enabled")),
           cJSON_IsNumber(reason) ? reason->valueint : 0, json_text(json, "error"));
  uart_emit(WireEvent, 0, event);
  cJSON_Delete(json);
}

static void networks(void) {
  cJSON *json = network_status();
  if (!json) return;
  bool scanning = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "scanning"));
  char event[128];
  snprintf(event, sizeof(event), "wifi_networks\t%d\t%s", scanning,
           json_text(json, "scan_error"));
  uart_emit(WireEvent, 0, event);
  if (!scanning) {
    const cJSON *entry;
    unsigned index = 0;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(json, "networks")) {
      if (index == 16) break;
      char name[65];
      const char *ssid = json_text(entry, "ssid");
      rule_to_hex((const uint8_t *)ssid, strlen(ssid), name);
      const cJSON *rssi = cJSON_GetObjectItemCaseSensitive(entry, "rssi");
      snprintf(event, sizeof(event), "wifi_network\t%u\t%s\t%d\t%d", index++, name,
               cJSON_IsNumber(rssi) ? rssi->valueint : -127,
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "secured")));
      uart_emit(WireEvent, 0, event);
    }
    snprintf(event, sizeof(event), "wifi_networks_end\t%u", index);
    uart_emit(WireEvent, 0, event);
  }
  cJSON_Delete(json);
}

static const char *update(char **fields, size_t count) {
  if (count != 7) return "Invalid Wi-Fi settings";
  cJSON *patch = cJSON_CreateObject();
  if (!patch) return "Out of memory";
  cJSON_AddStringToObject(patch, "mode", fields[1]);
  const char *keys[] = {"ap_ssid", "ap_password", "station_ssid", "station_password"};
  const char *error = NULL;
  for (unsigned i = 0; i < 4; i++) {
    if ((i == 1 || i == 3) && !strcmp(fields[i + 2], "-")) continue;
    uint8_t decoded[64] = {0};
    size_t length;
    if (!rule_hex(fields[i + 2], decoded, sizeof(decoded) - 1, &length) ||
        memchr(decoded, 0, length)) {
      error = "Invalid credential encoding";
      break;
    }
    cJSON_AddStringToObject(patch, keys[i], (char *)decoded);
    memset(decoded, 0, sizeof(decoded));
  }
  if (strcmp(fields[6], "0") && strcmp(fields[6], "1")) error = "Invalid MCP setting";
  cJSON_AddBoolToObject(patch, "mcp_enabled", !strcmp(fields[6], "1"));
  if (!error) error = network_update(patch);
  cJSON_Delete(patch);
  if (!error) snapshot();
  return error;
}

bool uart_network_command(uint16_t id, char **fields, size_t count) {
  if (strncmp(fields[0], "wifi_", 5)) return false;
  const char *error = NULL;
  if (!strcmp(fields[0], "wifi_get")) snapshot();
  else if (!strcmp(fields[0], "wifi_scan")) error = network_scan();
  else if (!strcmp(fields[0], "wifi_networks")) networks();
  else if (!strcmp(fields[0], "wifi_key")) {
    char event[48] = "wifi_key\t";
    network_key(event + 9);
    uart_emit(WireEvent, 0, event);
    memset(event, 0, sizeof(event));
  } else if (!strcmp(fields[0], "wifi_set")) error = update(fields, count);
  else error = "Unknown Wi-Fi command";
  char response[160];
  snprintf(response, sizeof(response), "%s\t%s", error ? "error" : "ok",
           error ? error : "saved");
  uart_emit(WireReply, id, response);
  return true;
}
