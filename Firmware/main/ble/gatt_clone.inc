// Bluetooth callbacks own setup. Stop quiesces the stack before freeing these services.
static void clone_failed(const char *operation, int status) {
  if (shutdown_gatt_attack) return;
  shutdown_gatt_attack = true;
  clone.server_ready = false;
  esp_ble_gap_stop_advertising();
  char message[112];
  snprintf(message, sizeof(message), "Clone failed: %s (0x%x). Stop and retry.",
           operation, (unsigned)status);
  ESP_LOGE(TAG, "%s", message);
  gatt_send_output(message, false);
  gattAttackState = GATT_ATTACK_STATE_FAILED;
  gatt_send_state(gattAttackState);
}

static uint16_t clone_service_handles(size_t index) {
  const service_t *service = &clone.services[index];
  uint32_t last = service->start_handle;
  if (!last || service->end_handle < last) return 0;
  for (size_t i = 0; i < service->num_chars; i++) {
    const char_t *characteristic = &service->chars[i];
    if (characteristic->char_handle <= service->start_handle ||
        characteristic->char_handle > service->end_handle) return 0;
    if (characteristic->char_handle > last) last = characteristic->char_handle;
    for (size_t j = 0; j < characteristic->num_descs; j++) {
      uint16_t handle = characteristic->descs[j].desc_handle;
      if (handle <= characteristic->char_handle || handle > service->end_handle) return 0;
      if (handle > last) last = handle;
    }
  }
  if (index + 1 < clone.num_services) {
    uint32_t next = clone.services[index + 1].start_handle;
    if (next <= service->end_handle) return 0;
    last = next - 1; // Preserve gaps so forwarded attribute handles remain aligned.
  }
  // A remote final range may end at 0xffff without having an attribute there.
  // Bluedroid stores its exclusive end in 16 bits; reserving through 0xffff wraps it.
  if (last >= UINT16_MAX) return 0;
  return (uint16_t)(last - service->start_handle + 1);
}

static void clone_create_services() {
  if (!clone.num_services) {
    clone_failed("empty service database", ESP_GATT_INVALID_HANDLE);
    return;
  }
  clone.server_ready = false;
  cloned_service_num = 0;
  for (size_t i = 0; i < clone.num_services; i++) {
    service_t *service = &clone.services[i];
    uint16_t handles = clone_service_handles(i);
    if (!handles) {
      clone_failed("unsupported service handle range", ESP_GATT_INVALID_HANDLE);
      return;
    }
    service->service_handle = 0;
    service->started = false;
    service->service_id = (esp_gatt_srvc_id_t){0};
    service->service_id.is_primary = true;
    service->service_id.id.inst_id = (uint8_t)i;
    service->service_id.id.uuid = service->service_uuid;
    ESP_LOGI(TAG, "Clone service %u/%u: remote %u..%u, reserve %u handles",
             (unsigned)i + 1, clone.num_services, service->start_handle,
             service->end_handle, handles);
    esp_err_t result = esp_ble_gatts_create_service(clone.gatts_if, &service->service_id, handles);
    if (result != ESP_OK) {
      clone_failed("create service submission", result);
      return;
    }
  }
}

static void clone_register(esp_gatt_if_t interface, esp_ble_gatts_cb_param_t *param) {
  if (param->reg.status != ESP_GATT_OK) {
    clone_failed("server registration", param->reg.status);
    return;
  }
  clone.gatts_if = interface;
  clone_create_services();
}

static void clone_created(esp_ble_gatts_cb_param_t *param) {
  if (param->create.status != ESP_GATT_OK) {
    clone_failed("create service", param->create.status);
    return;
  }
  unsigned index = param->create.service_id.id.inst_id;
  if (index >= clone.num_services) {
    clone_failed("unexpected service instance", ESP_GATT_INVALID_HANDLE);
    return;
  }
  service_t *service = &clone.services[index];
  if (service->service_handle) return;
  service->service_handle = param->create.service_handle;
  for (size_t i = 0; i < service->num_chars; i++) {
    char_t *characteristic = &service->chars[i];
    esp_err_t result = esp_ble_gatts_add_char(service->service_handle,
        &characteristic->char_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
        characteristic->property, &characteristic->value, NULL);
    if (result != ESP_OK) {
      clone_failed("add characteristic submission", result);
      return;
    }
    for (size_t j = 0; j < characteristic->num_descs; j++) {
      desc_t *descriptor = &characteristic->descs[j];
      result = esp_ble_gatts_add_char_descr(service->service_handle, &descriptor->desc_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, &descriptor->value, NULL);
      if (result != ESP_OK) {
        clone_failed("add descriptor submission", result);
        return;
      }
    }
  }
  esp_err_t result = esp_ble_gatts_start_service(service->service_handle);
  if (result != ESP_OK) clone_failed("start service submission", result);
}

static void clone_started(esp_ble_gatts_cb_param_t *param) {
  if (param->start.status != ESP_GATT_OK) {
    clone_failed("start service", param->start.status);
    return;
  }
  for (size_t i = 0; i < clone.num_services; i++) {
    service_t *service = &clone.services[i];
    if (service->service_handle != param->start.service_handle) continue;
    if (service->started) return;
    service->started = true;
    cloned_service_num++;
    ESP_LOGI(TAG, "Clone service started: %u/%u", cloned_service_num, clone.num_services);
    if (cloned_service_num == clone.num_services) {
      esp_err_t result = esp_ble_gap_start_advertising(&adv_params);
      if (result != ESP_OK) {
        clone_failed("advertising submission", result);
        return;
      }
      clone.server_ready = true;
      clone.refresh_pending = false;
      gatt_send_output("GATT Clone Success", false);
      gattAttackState = GATT_ATTACK_STATE_RUNNING;
      gatt_send_state(gattAttackState);
    }
    return;
  }
  clone_failed("unknown started service", ESP_GATT_INVALID_HANDLE);
}
