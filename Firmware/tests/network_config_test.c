#include "network_config.h"
#include <assert.h>
#include <string.h>

static const char *patch(NetworkConfig *config, const char *text) {
  cJSON *json = cJSON_Parse(text);
  const char *error = network_config_patch(config, json);
  cJSON_Delete(json);
  return error;
}

int main(void) {
  const uint8_t old_default[] = {0x13, 0x33, 0x77, 0x13, 0x37, 0x67};
  const uint8_t factory[] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
  const uint8_t local[] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
  const uint8_t zero[6] = {0}, broadcast[6] = {255, 255, 255, 255, 255, 255};
  assert(!network_mac_valid(old_default) && !network_mac_valid(zero));
  assert(!network_mac_valid(broadcast));
  assert(network_mac_valid(factory) && network_mac_valid(local));
  NetworkConfig config = {.version = 1,
                          .mode = "ap",
                          .ap_ssid = "Research board",
                          .ap_password = "existing-secret",
                          .key = "private-access-key"};
  NetworkConfig before = config;
  assert(patch(&config, "{\"mode\":\"station\"}"));
  assert(!memcmp(&before, &config, sizeof(config)));
  assert(patch(&config, "{\"ap_password\":\"short\"}"));
  assert(!memcmp(&before, &config, sizeof(config)));
  assert(patch(&config, "{\"key\":\"replacement\"}"));
  assert(patch(&config, "{\"mcp_enabled\":1}"));
  assert(patch(&config, "{\"ap_ssid\":\"line\\nbreak\"}"));
  assert(!patch(&config, "{\"mode\":\"both\",\"station_ssid\":\"Lab \\\"2\\\"\","
                         "\"station_password\":\"password!\",\"mcp_enabled\":true}"));
  assert(!strcmp(config.ap_password, "existing-secret") && config.mcp_enabled);
  assert(!strcmp(config.station_ssid, "Lab \"2\""));
  assert(!patch(&config, "{\"station_password\":\"\"}"));
  assert(!config.station_password[0]);
  assert(!patch(&config, "{\"ap_ssid\":\"12345678901234567890123456789012\"}"));
  before = config;
  assert(patch(&config, "{\"ap_ssid\":\"123456789012345678901234567890123\"}"));
  assert(!memcmp(&before, &config, sizeof(config)));
  assert(patch(&config, "{\"mode\":\"unknown\"}"));
  assert(patch(&config, "[]"));
}
