#include "gatt_internal.h"

// IDF's public read API assembles the entire target value, including Read Blob replies.
// Keep the central's offset separately, then slice the hooked value for its own MTU.
bool att_busy() {
  return pending_write.active || clone.transactions.canceling ||
      clone.transactions.request.operation != AttNone;
}

esp_gatt_status_t submission_status(esp_err_t result) {
  return result == ESP_ERR_NO_MEM ? ESP_GATT_NO_RESOURCES : ESP_GATT_ERROR;
}

void att_reply(AttRequest request, esp_gatt_status_t status,
                      esp_gatt_rsp_t *response) {
  if (request.abandoned) return;
  esp_err_t result = esp_ble_gatts_send_response(clone.gatts_if, request.central,
                                                request.transaction, status, response);
  if (result != ESP_OK) ESP_LOGW(TAG, "ATT response failed: %s", esp_err_to_name(result));
}

void log_att(AttRequest request, const char *operation,
                    esp_gatt_status_t status, const uint8_t *data, size_t length,
                    bool response) {
  char uuid[38] = {0};
  get_uuid_for_handle(request.handle, uuid);
  char *hex = length ? bytes_to_hex_string(data, length) : NULL;
  log_and_send_json(operation, response ? "target -> central" : "central -> target",
                    request.descriptor ? "DESC" : "CHAR",
                    clone.gattc_if, request.target, request.transaction, request.handle,
                    status, uuid, hex);
  free(hex);
}

void start_read(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
  const typeof(param->read) *read = &param->read;
  if (!connected) {
    reply_cached_read(gatts_if, param);
    return;
  }
  uint16_t target_handle = clone_target_handle(read->handle);
  AttRequest request = {AttRead, read->conn_id, clone.target_client_conn_id,
                        read->trans_id, target_handle, read->offset,
                        handle_is_descriptor(target_handle), false, 0};
  request.central_handle = read->handle;
  log_att(request, "READ", ESP_GATT_OK, NULL, 0, false);
  if (!target_handle) {
    log_att(request, "READ RSP", ESP_GATT_INVALID_HANDLE, NULL, 0, true);
    att_reply(request, ESP_GATT_INVALID_HANDLE, NULL);
    return;
  }
  if (att_busy()) {
    log_att(request, "READ RSP", ESP_GATT_BUSY, NULL, 0, true);
    att_reply(request, ESP_GATT_BUSY, NULL);
    return;
  }
  clone.transactions.request = request;
  esp_err_t result = request.descriptor
      ? esp_ble_gattc_read_char_descr(clone.gattc_if, request.target, target_handle,
                                      ESP_GATT_AUTH_REQ_NONE)
      : esp_ble_gattc_read_char(clone.gattc_if, request.target, target_handle,
                                ESP_GATT_AUTH_REQ_NONE);
  if (result != ESP_OK) {
    clone.transactions.request = (AttRequest){0};
    esp_gatt_status_t status = submission_status(result);
    log_att(request, "READ RSP", status, NULL, 0, true);
    att_reply(request, status, NULL);
  }
}

void finish_read(bool descriptor, esp_ble_gattc_cb_param_t *param) {
  const typeof(param->read) *read = &param->read;
  const AttRequest request = clone.transactions.request;
  if (request.operation != AttRead || request.descriptor != descriptor ||
      request.target != read->conn_id || request.handle != read->handle) return;
  clone.transactions.request = (AttRequest){0};
  if (request.abandoned) return;
  if (read->status != ESP_GATT_OK) {
    log_att(request, descriptor ? "READ DESC RSP" : "READ CHAR RSP",
            read->status, NULL, 0, true);
    att_reply(request, read->status, NULL);
    return;
  }
  if (read->value_len > ESP_GATT_MAX_ATTR_LEN || (read->value_len && !read->value)) {
    log_att(request, descriptor ? "READ DESC RSP" : "READ CHAR RSP",
            ESP_GATT_INVALID_ATTR_LEN, NULL, 0, true);
    att_reply(request, ESP_GATT_INVALID_ATTR_LEN, NULL);
    return;
  }
  uint16_t length = read->value_len;
  uint8_t *value = malloc(length ? length : 1);
  if (!value) {
    log_att(request, descriptor ? "READ DESC RSP" : "READ CHAR RSP",
            ESP_GATT_NO_RESOURCES, NULL, 0, true);
    att_reply(request, ESP_GATT_NO_RESOURCES, NULL);
    return;
  }
  if (length) memcpy(value, read->value, length);
  char uuid[38] = {0};
  get_uuid_for_handle(read->handle, uuid);
  char *hex = bytes_to_hex_string(value, length);
  apply_hook("read", uuid, &hex, &value, &length);
  free(hex);
  esp_gatt_rsp_t response = {0};
  esp_gatt_status_t status = ESP_GATT_OK;
  if (request.offset > length) {
    status = ESP_GATT_INVALID_OFFSET;
  } else {
    response.attr_value.handle = request.central_handle;
    response.attr_value.offset = request.offset;
    uint16_t remaining = length - request.offset;
    uint16_t limit = clone.transactions.central_mtu - 1;
    response.attr_value.len = remaining < limit ? remaining : limit;
    if (response.attr_value.len)
      memcpy(response.attr_value.value, value + request.offset, response.attr_value.len);
  }
  free(value);
  char *response_hex = status == ESP_GATT_OK
      ? bytes_to_hex_string(response.attr_value.value, response.attr_value.len) : NULL;
  log_and_send_json(descriptor ? "READ DESC RSP" : "READ CHAR RSP", "target -> central", "",
                    clone.gattc_if, request.target, request.transaction, read->handle,
                    status, uuid, response_hex);
  free(response_hex);
  att_reply(request, status, status == ESP_GATT_OK ? &response : NULL);
}
