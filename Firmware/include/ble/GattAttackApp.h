#pragma once

#include <stdint.h>
#include "ble/gatt_app_api.h"

typedef struct {
  const char *action;
  int id;
  const char *hook_ret;
  uint32_t request_id;
  bool enable;
  const char *mac;
} GattAttackWebEventParams;

#ifdef __cplusplus
extern "C" {
#endif

void gatt_web_event(GattAttackWebEventParams *params);
void gatt_app_init(void);
bool gatt_parse_mac(const char *text, uint8_t address[6]);
char *gatt_state_json(int state);

#ifdef __cplusplus
}
#endif
