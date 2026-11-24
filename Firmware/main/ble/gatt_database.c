#include "gatt_internal.h"

static bool same_uuid(const esp_bt_uuid_t *left, const esp_bt_uuid_t *right) {
  if (left->len != right->len) return false;
  if (left->len == ESP_UUID_LEN_16) return left->uuid.uuid16 == right->uuid.uuid16;
  if (left->len == ESP_UUID_LEN_32) return left->uuid.uuid32 == right->uuid.uuid32;
  return left->len == ESP_UUID_LEN_128 && !memcmp(left->uuid.uuid128, right->uuid.uuid128, 16);
}

// Compare the freshly discovered cache, not attribute values that change during use.
// Reading one entry at a time bounds our scratch space on the Bluetooth task.
static bool same_characteristics(esp_gatt_if_t interface, uint16_t connection,
                                 const service_t *service) {
  uint16_t count = 0;
  esp_gatt_status_t status = esp_ble_gattc_get_attr_count(interface, connection,
      ESP_GATT_DB_CHARACTERISTIC, service->start_handle, service->end_handle, 0, &count);
  if (status != ESP_GATT_OK) {
    clone_failed("verify characteristics", status);
    return false;
  }
  if (count != service->num_chars) return false;
  for (uint16_t i = 0; i < count; i++) {
    esp_gattc_char_elem_t remote = {0};
    uint16_t one = 1;
    status = esp_ble_gattc_get_all_char(interface, connection, service->start_handle,
        service->end_handle, &remote, &one, i);
    // This IDF version returns the total available count, not the copied count.
    if (status != ESP_GATT_OK || !one) {
      clone_failed("verify characteristic entry", status);
      return false;
    }
    const char_t *local = &service->chars[i];
    if (remote.char_handle != local->char_handle || remote.properties != local->property ||
        !same_uuid(&remote.uuid, &local->char_uuid)) return false;
    uint16_t descriptors = 0;
    status = esp_ble_gattc_get_attr_count(interface, connection, ESP_GATT_DB_DESCRIPTOR,
                                         0, 0, local->char_handle, &descriptors);
    if (status != ESP_GATT_OK) {
      clone_failed("verify descriptors", status);
      return false;
    }
    if (descriptors != local->num_descs) return false;
    for (uint16_t j = 0; j < descriptors; j++) {
      esp_gattc_descr_elem_t descriptor = {0};
      one = 1;
      status = esp_ble_gattc_get_all_descr(interface, connection, local->char_handle,
                                          &descriptor, &one, j);
      if (status != ESP_GATT_OK || !one) {
        clone_failed("verify descriptor entry", status);
        return false;
      }
      if (descriptor.handle != local->descs[j].desc_handle ||
          !same_uuid(&descriptor.uuid, &local->descs[j].desc_uuid)) return false;
    }
  }
  return true;
}

void clone_verify_service(esp_gatt_if_t interface, esp_ble_gattc_cb_param_t *param) {
  unsigned index = clone.verified_services++;
  if (clone.database_changed) return;
  if (index >= clone.num_services) {
    clone.database_changed = true;
    return;
  }
  const service_t *service = &clone.services[index];
  const typeof(param->search_res) *remote = &param->search_res;
  clone.database_changed = remote->start_handle != service->start_handle ||
      remote->end_handle != service->end_handle ||
      !same_uuid(&remote->srvc_id.uuid, &service->service_uuid) ||
      !same_characteristics(interface, remote->conn_id, service);
}

static void clone_refresh_delete_next(void) {
  if (shutdown_gatt_attack || !clone.refresh_pending) return;
  while (clone.refresh_index < clone.num_services) {
    service_t *service = &clone.services[clone.refresh_index++];
    esp_err_t result = esp_ble_gatts_delete_service(service->service_handle);
    if (result != ESP_OK) clone_failed("delete service submission", result);
    return;
  }
  if (!connected) {
    clone_failed("target lost during service refresh", ESP_GATT_ERROR);
    return;
  }
  free_clone();
  clone_init = false;
  service_num = cloned_service_num = last_end_handle = 0;
  // Search reads the already discovered remote cache; it does not reconnect.
  esp_err_t result = esp_ble_gattc_search_service(clone.gattc_if,
                                                 clone.target_client_conn_id, NULL);
  if (result != ESP_OK) clone_failed("refresh service search", result);
}

// These operations run on the Bluetooth callback task. Delete completions fence the
// old database's lifetime; the target ACL and its pending MTU exchange stay alive.
void clone_service_deleted(esp_ble_gatts_cb_param_t *param) {
  if (shutdown_gatt_attack || !clone.refresh_pending || !clone.refresh_index) return;
  for (unsigned i = 0; i < clone.num_services; i++) {
    service_t *service = &clone.services[i];
    if (service->service_handle != param->del.service_handle) continue;
    if (param->del.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "Delete service %u UUID len %u status 0x%x", service->service_handle,
               service->service_uuid.len, param->del.status);
      clone_failed("delete old service", param->del.status);
      return;
    }
    service->service_handle = 0;
    clone_refresh_delete_next();
    return;
  }
}

void clone_refresh_central_disconnected(void) {
  if (!clone.refresh_pending || !clone.refresh_wait_central) return;
  clone.refresh_wait_central = false;
  clone_refresh_delete_next();
}

void clone_verify_complete() {
  if (shutdown_gatt_attack || clone.refresh_pending) return;
  if (!clone.database_changed && clone.verified_services == clone.num_services) {
    if (!clone.handoff_pending) return;
    esp_err_t result = esp_ble_gap_start_advertising(&adv_params);
    if (result != ESP_OK) {
      clone_failed("resume advertising after handoff", result);
      return;
    }
    clone.handoff_pending = false;
    clone.server_ready = true;
    gattAttackState = GATT_ATTACK_STATE_RUNNING;
    gatt_send_state(gattAttackState);
    return;
  }
  clone.refresh_pending = true;
  clone.refresh_index = 0;
  clone.server_ready = false;
  esp_ble_gap_stop_advertising();
  gatt_send_output("Target services changed; refreshing server", false);
  gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECTED;
  gatt_send_state(gattAttackState);
  if (uart_bridge_phone_connected()) {
    clone.refresh_wait_central = true;
    esp_err_t result = esp_ble_gap_disconnect(clone.central_bda);
    if (result != ESP_OK) {
      clone_failed("disconnect central for rediscovery", result);
      return;
    }
    return;
  }
  clone_refresh_delete_next();
}
