// A prepare response echoes the chunk only after the target accepts it. No assembled
// scratch buffer is needed: the target owns the queued attributes until execute/cancel.
static void start_prepare(esp_ble_gatts_cb_param_t *param) {
  const typeof(param->write) *write = &param->write;
  GattTransactions *state = &clone.transactions;
  AttRequest request = {AttPrepare, write->conn_id, clone.target_client_conn_id,
                        write->trans_id, write->handle, write->offset,
                        handle_is_descriptor(write->handle), false};
  if (!connected || att_busy() || (state->queued && state->prepared_owner != write->conn_id)) {
    att_reply(request, ESP_GATT_BUSY, NULL);
    return;
  }
  if (write->offset > ESP_GATT_MAX_ATTR_LEN ||
      write->len > ESP_GATT_MAX_ATTR_LEN - write->offset || (write->len && !write->value)) {
    att_reply(request, ESP_GATT_INVALID_ATTR_LEN, NULL);
    return;
  }
  state->prepared = (esp_gatt_rsp_t){0};
  state->prepared.attr_value.handle = write->handle;
  state->prepared.attr_value.offset = write->offset;
  state->prepared.attr_value.len = write->len;
  if (write->len) memcpy(state->prepared.attr_value.value, write->value, write->len);
  state->request = request;
  log_att(request, "PREP WRITE", ESP_GATT_OK, write->value, write->len, false);
  esp_err_t result = request.descriptor
      ? esp_ble_gattc_prepare_write_char_descr(clone.gattc_if, request.target, write->handle,
          write->offset, write->len, write->value, ESP_GATT_AUTH_REQ_NONE)
      : esp_ble_gattc_prepare_write(clone.gattc_if, request.target, write->handle,
          write->offset, write->len, write->value, ESP_GATT_AUTH_REQ_NONE);
  if (result != ESP_OK) {
    state->request = (AttRequest){0};
    att_reply(request, submission_status(result), NULL);
    return;
  }
  state->queued = true;
  state->prepared_owner = write->conn_id;
}

static void finish_prepare(esp_ble_gattc_cb_param_t *param) {
  GattTransactions *state = &clone.transactions;
  const AttRequest request = state->request;
  const typeof(param->write) *write = &param->write;
  if (request.operation != AttPrepare || request.target != write->conn_id ||
      request.handle != write->handle || request.offset != write->offset) return;
  state->request = (AttRequest){0};
  log_att(request, "PREP WRITE RSP", write->status, state->prepared.attr_value.value,
          state->prepared.attr_value.len, true);
  att_reply(request, write->status, write->status == ESP_GATT_OK ? &state->prepared : NULL);
}

static void start_execute(esp_ble_gatts_cb_param_t *param) {
  GattTransactions *state = &clone.transactions;
  const typeof(param->exec_write) *execute = &param->exec_write;
  AttRequest request = {AttExecute, execute->conn_id, clone.target_client_conn_id,
                        execute->trans_id, 0, 0, false, false};
  if (!connected || att_busy() || (state->queued && state->prepared_owner != execute->conn_id)) {
    att_reply(request, ESP_GATT_BUSY, NULL);
    return;
  }
  state->request = request;
  bool execute_write = execute->exec_write_flag == ESP_GATT_PREP_WRITE_EXEC;
  log_att(request, execute_write ? "EXEC WRITE" : "ABORT WRITE", ESP_GATT_OK, NULL, 0, false);
  esp_err_t result = esp_ble_gattc_execute_write(clone.gattc_if, request.target,
                                                execute_write);
  if (result != ESP_OK) {
    state->request = (AttRequest){0};
    att_reply(request, submission_status(result), NULL);
  }
}

static void finish_execute(esp_ble_gattc_cb_param_t *param) {
  GattTransactions *state = &clone.transactions;
  const typeof(param->exec_cmpl) *execute = &param->exec_cmpl;
  if (execute->conn_id != clone.target_client_conn_id) return;
  if (state->canceling) {
    state->canceling = false;
    state->queued = false;
    return;
  }
  const AttRequest request = state->request;
  if (request.operation != AttExecute || request.target != execute->conn_id) return;
  state->request = (AttRequest){0};
  state->queued = false;
  log_att(request, "EXEC RSP", execute->status, NULL, 0, true);
  att_reply(request, execute->status, NULL);
}

static void central_transactions_disconnected(uint16_t connection) {
  GattTransactions *state = &clone.transactions;
  if (state->request.operation != AttNone && state->request.central == connection)
    state->request.abandoned = true;
  if (!state->queued || state->prepared_owner != connection || !connected) return;
  // Drain late completions before another central can reuse the target's prepare queue.
  // An in-flight execute already owns the queue and will release it on completion.
  if (state->request.operation == AttExecute) return;
  state->canceling = true;
  if (esp_ble_gattc_execute_write(clone.gattc_if, clone.target_client_conn_id, false) != ESP_OK)
    esp_ble_gattc_close(clone.gattc_if, clone.target_client_conn_id);
}

static void target_transactions_disconnected() {
  GattTransactions *state = &clone.transactions;
  const AttRequest request = state->request;
  state->request = (AttRequest){0};
  state->queued = state->canceling = false;
  if (request.operation != AttNone) att_reply(request, ESP_GATT_ERROR, NULL);
}
