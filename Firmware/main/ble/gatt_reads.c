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
  AttRequest request = {AttRead, read->conn_id, clone.target_client_conn_id,
                        read->trans_id, read->handle, read->offset,
                        handle_is_descriptor(read->handle), false};
  if (att_busy()) {
    att_reply(request, ESP_GATT_BUSY, NULL);
    return;
  }
  clone.transactions.request = request;
  char uuid[38] = {0};
  get_uuid_for_handle(read->handle, uuid);
  log_and_send_json("READ", "central -> target", request.descriptor ? "DESC" : "CHAR",
                    clone.gattc_if, request.target, request.transaction, read->handle,
                    ESP_GATT_OK, uuid, NULL);
  esp_err_t result = request.descriptor
      ? esp_ble_gattc_read_char_descr(clone.gattc_if, request.target, read->handle,
                                      ESP_GATT_AUTH_REQ_NONE)
      : esp_ble_gattc_read_char(clone.gattc_if, request.target, read->handle,
                                ESP_GATT_AUTH_REQ_NONE);
  if (result != ESP_OK) {
    clone.transactions.request = (AttRequest){0};
    att_reply(request, submission_status(result), NULL);
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
    att_reply(request, ESP_GATT_INVALID_ATTR_LEN, NULL);
    return;
  }
  uint16_t length = read->value_len;
  uint8_t *value = malloc(length ? length : 1);
  if (!value) {
    att_reply(request, ESP_GATT_NO_RESOURCES, NULL);
    return;
  }
  if (length) memcpy(value, read->value, length);
  char uuid[38] = {0};
  get_uuid_for_handle(read->handle, uuid);
  char *hex = bytes_to_hex_string(value, length);
  apply_hook("read", uuid, &hex, &value, &length);
  log_and_send_json(descriptor ? "READ DESC RSP" : "READ CHAR RSP", "target -> central", "",
                    clone.gattc_if, request.target, request.transaction, read->handle,
                    read->status, uuid, hex);
  free(hex);
  esp_gatt_rsp_t response = {0};
  esp_gatt_status_t status = ESP_GATT_OK;
  if (request.offset > length) {
    status = ESP_GATT_INVALID_OFFSET;
  } else {
    response.attr_value.handle = request.handle;
    response.attr_value.offset = request.offset;
    uint16_t remaining = length - request.offset;
    uint16_t limit = clone.transactions.central_mtu - 1;
    response.attr_value.len = remaining < limit ? remaining : limit;
    if (response.attr_value.len)
      memcpy(response.attr_value.value, value + request.offset, response.attr_value.len);
  }
  free(value);
  att_reply(request, status, status == ESP_GATT_OK ? &response : NULL);
}
