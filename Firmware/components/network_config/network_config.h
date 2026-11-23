#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint32_t version;
  char mode[8];
  char ap_ssid[33];
  char ap_password[64];
  char station_ssid[33];
  char station_password[64];
  char key[33];
  bool mcp_enabled;
} NetworkConfig;
const char *network_config_patch(NetworkConfig *config, const cJSON *patch);
bool network_mac_valid(const uint8_t mac[6]);
