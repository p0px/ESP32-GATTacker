#include "gatt_internal.h"

static bool same_uuid(const esp_bt_uuid_t *left, const esp_bt_uuid_t *right) {
  if (left->len != right->len) return false;
  if (left->len == ESP_UUID_LEN_16) return left->uuid.uuid16 == right->uuid.uuid16;
  if (left->len == ESP_UUID_LEN_32) return left->uuid.uuid32 == right->uuid.uuid32;
  return left->len == ESP_UUID_LEN_128 &&
      !memcmp(left->uuid.uuid128, right->uuid.uuid128, 16);
}

static service_t *local_service(uint16_t handle) {
  for (size_t i = 0; i < clone.num_services; i++) {
    if (clone.services[i].service_handle == handle) return &clone.services[i];
  }
  return NULL;
}

// Add callbacks report the actual server handles, which can differ from target handles.
bool clone_char_added(const esp_ble_gatts_cb_param_t *param) {
  service_t *service = local_service(param->add_char.service_handle);
  if (!service) return false;
  for (size_t i = 0; i < service->num_chars; i++) {
    char_t *characteristic = &service->chars[i];
    if (characteristic->local_handle ||
        !same_uuid(&characteristic->char_uuid, &param->add_char.char_uuid)) continue;
    characteristic->local_handle = param->add_char.attr_handle;
    ESP_LOGI(TAG, "Characteristic handle %u -> %u", characteristic->local_handle,
             characteristic->char_handle);
    return true;
  }
  return false;
}

bool clone_descr_added(const esp_ble_gatts_cb_param_t *param) {
  service_t *service = local_service(param->add_char_descr.service_handle);
  if (!service) return false;
  for (size_t i = 0; i < service->num_chars; i++) {
    char_t *characteristic = &service->chars[i];
    for (size_t j = 0; j < characteristic->num_descs; j++) {
      desc_t *descriptor = &characteristic->descs[j];
      if (descriptor->local_handle ||
          !same_uuid(&descriptor->desc_uuid, &param->add_char_descr.descr_uuid)) continue;
      descriptor->local_handle = param->add_char_descr.attr_handle;
      ESP_LOGI(TAG, "Descriptor handle %u -> %u", descriptor->local_handle,
               descriptor->desc_handle);
      return true;
    }
  }
  return false;
}

uint16_t clone_target_handle(uint16_t local_handle) {
  for (size_t i = 0; i < clone.num_services; i++) {
    const service_t *service = &clone.services[i];
    for (size_t j = 0; j < service->num_chars; j++) {
      const char_t *characteristic = &service->chars[j];
      if (characteristic->local_handle == local_handle) return characteristic->char_handle;
      for (size_t k = 0; k < characteristic->num_descs; k++) {
        const desc_t *descriptor = &characteristic->descs[k];
        if (descriptor->local_handle == local_handle) return descriptor->desc_handle;
      }
    }
  }
  return 0;
}

uint16_t clone_local_handle(uint16_t target_handle) {
  for (size_t i = 0; i < clone.num_services; i++) {
    const service_t *service = &clone.services[i];
    for (size_t j = 0; j < service->num_chars; j++) {
      const char_t *characteristic = &service->chars[j];
      if (characteristic->char_handle == target_handle) return characteristic->local_handle;
      for (size_t k = 0; k < characteristic->num_descs; k++) {
        const desc_t *descriptor = &characteristic->descs[k];
        if (descriptor->desc_handle == target_handle) return descriptor->local_handle;
      }
    }
  }
  return 0;
}
