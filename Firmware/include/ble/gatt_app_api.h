#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GattAttackState {
  GATT_ATTACK_STATE_HOME,
  GATT_ATTACK_STATE_SCANNING,
  GATT_ATTACK_STATE_SCAN_RES,
  GATT_ATTACK_STATE_START,
  GATT_ATTACK_STATE_CLIENT_CONNECTING,
  GATT_ATTACK_STATE_CLIENT_CONNECT_FAIL,
  GATT_ATTACK_STATE_CLIENT_CONNECTED,
  GATT_ATTACK_STATE_RUNNING,
  GATT_ATTACK_STATE_FAILED,
} GattAttackState;

extern GattAttackState gattAttackState;
bool should_run_hooks(void);
char *run_hook(const char *type, const char *uuid, const char *value);
void gatt_send_output(const char *output, bool is_json);
void gatt_send_state(int state);

#ifdef __cplusplus
}
#endif
