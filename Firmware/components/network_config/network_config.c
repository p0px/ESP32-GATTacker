#include "network_config.h"
#include <string.h>

bool network_mac_valid(const uint8_t mac[6]) {
  unsigned nonzero = 0;
  for (unsigned i = 0; i < 6; i++) nonzero |= mac[i];
  return !(mac[0] & 1) && nonzero; // Wi-Fi station addresses must be nonzero unicast.
}

static bool text(const cJSON *patch, const char *name, char *out, size_t capacity) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(patch, name);
  if (!item)
    return true;
  if (!cJSON_IsString(item) || strlen(item->valuestring) >= capacity)
    return false;
  for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p)
    if (*p < 32 || *p == 127)
      return false;
  strcpy(out, item->valuestring);
  return true;
}

const char *network_config_patch(NetworkConfig *config, const cJSON *patch) {
  if (!cJSON_IsObject(patch))
    return "Expected a settings object";
  NetworkConfig next = *config;
  const cJSON *field;
  cJSON_ArrayForEach(field, patch) {
    const char *name = field->string;
    if (!name || (strcmp(name, "mode") && strcmp(name, "ap_ssid") && strcmp(name, "ap_password") &&
                  strcmp(name, "station_ssid") && strcmp(name, "station_password") &&
                  strcmp(name, "mcp_enabled")))
      return "Unknown Wi-Fi setting";
  }
  if (!text(patch, "mode", next.mode, sizeof(next.mode)) ||
      !text(patch, "ap_ssid", next.ap_ssid, sizeof(next.ap_ssid)) ||
      !text(patch, "ap_password", next.ap_password, sizeof(next.ap_password)) ||
      !text(patch, "station_ssid", next.station_ssid, sizeof(next.station_ssid)) ||
      !text(patch, "station_password", next.station_password, sizeof(next.station_password)))
    return "SSID: at most 32 bytes; password: at most 63 bytes; no control characters";
  if (strcmp(next.mode, "ap") && strcmp(next.mode, "station") && strcmp(next.mode, "both"))
    return "Choose ap, station, or both";
  if (!next.ap_ssid[0] || strlen(next.ap_password) < 8)
    return "Setup AP needs a name and an 8-63 byte password";
  if (strcmp(next.mode, "ap") && !next.station_ssid[0])
    return "Enter the network SSID before joining";
  size_t length = strlen(next.station_password);
  if (length && length < 8)
    return "Network password must be empty or 8-63 bytes";
  field = cJSON_GetObjectItemCaseSensitive(patch, "mcp_enabled");
  if (field && !cJSON_IsBool(field))
    return "mcp_enabled must be true or false";
  if (field)
    next.mcp_enabled = cJSON_IsTrue(field);
  *config = next;
  return NULL;
}
