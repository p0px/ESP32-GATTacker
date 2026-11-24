#include "esp_heap_caps.h"

// All queue operations run on Bluedroid's GATT callback task. A successful
// submission owns one callback; queued phone writes wait for that callback.
static const size_t WRITE_QUEUE_LIMIT = 1024 * 1024;
static const unsigned WRITE_COUNT_LIMIT = 2048;

static void write_queue_reset();

static void write_queue_pace_central() {
  GattWriteQueue *queue = &clone.writes;
  if (queue->pacing_requested || queue->count < 64) return;
  esp_gap_conn_params_t target = {0};
  if (esp_ble_get_current_conn_params(clone.target_bda, &target) != ESP_OK ||
      !target.interval) return;
  uint16_t interval = target.interval + target.interval / 2;
  if (interval < 0x30) interval = 0x30;
  if (interval > 0x50) interval = 0x50;
  esp_ble_conn_update_params_t request = {0};
  memcpy(request.bda, clone.central_bda, sizeof(request.bda));
  request.min_int = request.max_int = interval;
  request.latency = 0;
  request.timeout = 400;
  if (esp_ble_gap_update_conn_params(&request) == ESP_OK) {
    queue->pacing_requested = true;
    ESP_LOGI(TAG, "Target write backlog %u; requested central interval %u units",
             queue->count, interval);
  }
}

static void write_queue_pump() {
  GattWriteQueue *queue = &clone.writes;
  if (!connected || queue->in_flight || queue->congested || !queue->head) return;
  GattQueuedWrite *write = queue->head;
  esp_gatt_write_type_t type = write->response ? ESP_GATT_WRITE_TYPE_RSP
                                               : ESP_GATT_WRITE_TYPE_NO_RSP;
  esp_err_t result = write->descriptor
      ? esp_ble_gattc_write_char_descr(clone.gattc_if, clone.target_client_conn_id,
                                      write->handle, write->length, write->value, type,
                                      ESP_GATT_AUTH_REQ_NONE)
      : esp_ble_gattc_write_char(clone.gattc_if, clone.target_client_conn_id,
                                write->handle, write->length, write->value, type,
                                ESP_GATT_AUTH_REQ_NONE);
  if (result == ESP_OK) {
    queue->in_flight = true;
  } else if (result == ESP_FAIL) {
    // ESP_FAIL here is the SDK's L2CAP congestion check. Keep the bytes until
    // CONGEST_EVT or QUEUE_FULL_EVT reports that the target link can drain.
    queue->congested = true;
    ESP_LOGW(TAG, "Target write paused: %s", esp_err_to_name(result));
  } else {
    ESP_LOGE(TAG, "Target write rejected: %s", esp_err_to_name(result));
    if (write->response) finish_pending_write(ESP_GATT_ERROR);
    else esp_ble_gap_disconnect(clone.central_bda);
    write_queue_reset();
  }
}

static bool write_queue_add(uint16_t central, uint16_t handle, bool descriptor,
                            bool response, const uint8_t *value, uint16_t length) {
  GattWriteQueue *queue = &clone.writes;
  size_t allocation = sizeof(GattQueuedWrite) + length;
  if (queue->count >= WRITE_COUNT_LIMIT || allocation > WRITE_QUEUE_LIMIT - queue->bytes)
    return false;
  uint32_t caps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM)
      ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  GattQueuedWrite *write = heap_caps_malloc(allocation, caps);
  if (!write) return false;
  write->next = NULL;
  write->central = central;
  write->handle = handle;
  write->length = length;
  write->descriptor = descriptor;
  write->response = response;
  if (length) memcpy(write->value, value, length);
  if (queue->tail) queue->tail->next = write;
  else queue->head = write;
  queue->tail = write;
  queue->bytes += allocation;
  queue->count++;
  write_queue_pace_central();
  if (queue->count > queue->peak) {
    queue->peak = queue->count;
    if (queue->peak >= 64 && !(queue->peak & (queue->peak - 1)))
      ESP_LOGI(TAG, "Target write backlog: %u entries", queue->peak);
  }
  write_queue_pump();
  return true;
}

static void write_queue_pop() {
  GattWriteQueue *queue = &clone.writes;
  GattQueuedWrite *write = queue->head;
  if (!write) return;
  queue->head = write->next;
  if (!queue->head) queue->tail = NULL;
  queue->bytes -= sizeof(GattQueuedWrite) + write->length;
  queue->count--;
  free(write);
}

static void write_queue_reset() {
  while (clone.writes.head) write_queue_pop();
  clone.writes.in_flight = false;
  clone.writes.congested = false;
  clone.writes.pacing_requested = false;
}

static void write_queue_target_disconnected() {
  if (!pending_write.active) return;
  // The target may reboot immediately after accepting a control write. A
  // local GATT_ERROR callback is not a target ATT error; relay the link loss.
  pending_write.active = false;
  write_queue_reset();
  esp_err_t result = esp_ble_gap_disconnect(clone.central_bda);
  if (result != ESP_OK)
    ESP_LOGW(TAG, "Central disconnect after target loss: %s", esp_err_to_name(result));
}

static void write_queue_central_disconnected(uint16_t central) {
  GattWriteQueue *queue = &clone.writes;
  if (!queue->in_flight || !queue->head || queue->head->central != central) {
    write_queue_reset();
    return;
  }
  GattQueuedWrite *active = queue->head;
  queue->head = active->next;
  while (queue->head) write_queue_pop();
  active->next = NULL;
  queue->head = queue->tail = active;
  queue->bytes = sizeof(GattQueuedWrite) + active->length;
  queue->count = 1;
  queue->pacing_requested = false;
}

static void write_queue_complete(bool descriptor, esp_ble_gattc_cb_param_t *param) {
  GattWriteQueue *queue = &clone.writes;
  GattQueuedWrite *write = queue->head;
  if (!queue->in_flight || !write || write->descriptor != descriptor ||
      write->handle != param->write.handle ||
      param->write.conn_id != clone.target_client_conn_id) return;
  queue->in_flight = false;
  if (param->write.status == ESP_GATT_CONGESTED) {
    // Bluedroid reports this for a write command already sent to L2CAP.
    // Replaying it duplicates firmware bytes. Wait for the link to drain.
    queue->congested = true;
    if (write->response && pending_write.active &&
        pending_write.conn_id == write->central &&
        pending_write.handle == write->handle)
      finish_pending_write(ESP_GATT_ERROR);
    write_queue_pop();
    return;
  }
  if (write->response && pending_write.active &&
      pending_write.conn_id == write->central &&
      pending_write.handle == write->handle)
    finish_pending_write(param->write.status);
  if (param->write.status != ESP_GATT_OK) {
    ESP_LOGE(TAG, "Target write failed: handle 0x%04x, status 0x%x",
             write->handle, param->write.status);
    if (!write->response) {
      esp_ble_gap_disconnect(clone.central_bda);
      write_queue_reset();
      return;
    }
  }
  write_queue_pop();
  write_queue_pump();
}

static void write_queue_resume(uint16_t target, bool congested) {
  if (target != clone.target_client_conn_id || congested) return;
  clone.writes.congested = false;
  write_queue_pump();
}
