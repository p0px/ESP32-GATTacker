#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Passwords are never included in status; an omitted password preserves its saved value.
void network_init(void);
cJSON *network_status(void);
const char *network_update(const cJSON *patch);
const char *network_scan(void);
bool network_authorized(const char *token);
void network_key(char out[33]);
const char *network_rotate_key(char out[33]);
bool network_mcp_enabled(void);
void network_endpoint(char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
