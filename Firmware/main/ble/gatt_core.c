#include "gatt_internal.h"
#include "ble/traffic_store.h"



// Used only while the target is disconnected. NULL means uncached, not an empty value.
static const esp_attr_value_t *cached_attribute(uint16_t handle) {
  for (unsigned i = 0; i < clone.num_services; i++) {
    const service_t *service = &clone.services[i];
    for (unsigned j = 0; j < service->num_chars; j++) {
      const char_t *characteristic = &service->chars[j];
      if (characteristic->char_handle == handle) return &characteristic->value;
      for (unsigned k = 0; k < characteristic->num_descs; k++) {
        const desc_t *descriptor = &characteristic->descs[k];
        if (descriptor->desc_handle == handle) return &descriptor->value;
      }
    }
  }
  return NULL;
}

void reply_cached_read(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
  uint16_t target_handle = clone_target_handle(param->read.handle);
  const esp_attr_value_t *value = cached_attribute(target_handle);
  char uuid[38] = {0};
  get_uuid_for_handle(target_handle, uuid);
  const char *kind = handle_is_descriptor(target_handle) ? "DESC" : "CHAR";
  log_and_send_json("READ", "central -> cache", kind, clone.gattc_if,
                    param->read.conn_id, param->read.trans_id, target_handle,
                    ESP_GATT_OK, uuid, NULL);
  esp_gatt_rsp_t response = {0};
  response.attr_value.handle = param->read.handle;
  response.attr_value.offset = param->read.offset;
  esp_gatt_status_t status = ESP_GATT_OK;
  if (!value || !value->attr_value)
    status = ESP_GATT_BUSY;
  else if (value->attr_len > sizeof(response.attr_value.value))
    status = ESP_GATT_INVALID_ATTR_LEN;
  else if (param->read.offset > value->attr_len)
    status = ESP_GATT_INVALID_OFFSET;
  else {
    response.attr_value.len = value->attr_len - param->read.offset;
    if (response.attr_value.len)
      memcpy(response.attr_value.value, value->attr_value + param->read.offset,
             response.attr_value.len);
  }
  // Never invent 0x01 as a successful attribute value. Appearance, for example, needs 2 bytes.
  esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                             status, status == ESP_GATT_OK ? &response : NULL);
  char *hex = status == ESP_GATT_OK
      ? bytes_to_hex_string(response.attr_value.value, response.attr_value.len) : NULL;
  log_and_send_json("READ RSP", "cache -> central", kind, clone.gattc_if,
                    param->read.conn_id, param->read.trans_id, target_handle,
                    status, uuid, hex);
  free(hex);
}


atomic_bool shutdown_gatt_attack = false;
int retries = 0;
int MAX_RETRIES = 3;
bool connected = false;
bool connecting = false;
bool clone_init = false; // if the cloned gatt structure has been completed
uint8_t service_num = 0; // number of services found
uint8_t cloned_service_num = 0; // number of cloned services
uint8_t last_end_handle = 0; // used for calculating the # of handles needed


pending_write_t pending_write;

void finish_pending_write(esp_gatt_status_t status) {
  if (!pending_write.active) return;
  const pending_write_t request = pending_write;
  pending_write.active = false;
  esp_err_t ret = esp_ble_gatts_send_response(clone.gatts_if, request.conn_id,
                                             request.trans_id, status, NULL);
  if (ret != ESP_OK) ESP_LOGW(TAG, "Write response failed: %s", esp_err_to_name(ret));
  char uuid[38] = {0};
  get_uuid_for_handle(request.handle, uuid);
  log_and_send_json("WRITE RSP", connected ? "target -> central" : "cache -> central",
                    request.descriptor ? "DESC" : "CHAR", clone.gattc_if, request.conn_id,
                    request.trans_id, request.handle, status, uuid, NULL);
  ESP_LOGI(TAG, "Write complete h:%u status:%u elapsed_ms:%llu", request.handle, status,
           (unsigned long long)((esp_timer_get_time() - request.started_us) / 1000));
}


/* Utils */

void log_and_send_json(
  const char *operation, const char *direction, const char *kind, uint32_t,
  uint32_t, uint32_t transaction, uint16_t handle, uint32_t status, const char *uuid,
  const char *extra_data) {
  traffic_store_record(operation, direction, kind, transaction, handle, status,
                       uuid, extra_data);
  uart_bridge_traffic(operation, direction, uuid, handle, extra_data);
}


/**
 * bytes_to_hex_string
 *
 * converts a uint8_t data
 * array and returns a char*
 *
 * @param uint8_t* data
 * @param size_t data_len
 * @return char* - hex string
 */
char* bytes_to_hex_string(const uint8_t *data, size_t data_len) {
  // Calculate necessary length for the hex string (2 chars per byte + null terminator)
  size_t hex_string_size = (data_len * 2) + 1;
  char *hex_string = (char*)malloc(hex_string_size);

  if (!hex_string) {
    ESP_LOGE(TAG, "Failed to allocate memory for hex string.");
    return NULL;
  }

  for (size_t i = 0; i < data_len; ++i) {
    sprintf(&hex_string[i * 2], "%02X", data[i]);
  }
  hex_string[data_len * 2] = '\0';

  return hex_string;
};

uint8_t* hex_string_to_bytes(const char *hex_string, uint16_t *out_len) {
  return decode_hex_payload(hex_string, out_len, ESP_GATT_MAX_ATTR_LEN);
}

void apply_hook(const char *type, const char *uuid, char **hex,
                       uint8_t **bytes, uint16_t *length) {
  if (!*hex) return;
  char *reply = uart_bridge_hook(type, uuid, *hex);
  if (!reply && should_run_hooks()) reply = run_hook(type, uuid, *hex);
  if (!reply) return;
  uint16_t new_length = *length;
  uint8_t *replacement = hex_string_to_bytes(reply, &new_length);
  if (!replacement) {
    free(reply);
    return;
  }
  free(*hex);
  free(*bytes);
  *hex = reply;
  *bytes = replacement;
  *length = new_length;
}

/**
 * print_uuid
 *
 * Prints a esp uuid to a char*
 * parameter.
 *
 * @param esp_bt_uuid_t uuid
 * @param char* out
 */
void print_uuid(esp_bt_uuid_t uuid, char* out) {
  switch (uuid.len) {
    case ESP_UUID_LEN_128: {
      snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid.uuid.uuid128[15], uuid.uuid.uuid128[14], uuid.uuid.uuid128[13], uuid.uuid.uuid128[12],
        uuid.uuid.uuid128[11], uuid.uuid.uuid128[10], uuid.uuid.uuid128[9], uuid.uuid.uuid128[8],
        uuid.uuid.uuid128[7], uuid.uuid.uuid128[6], uuid.uuid.uuid128[5], uuid.uuid.uuid128[4],
        uuid.uuid.uuid128[3], uuid.uuid.uuid128[2], uuid.uuid.uuid128[1], uuid.uuid.uuid128[0]);
      break;
    }
    case ESP_UUID_LEN_32: {
      sprintf(out, "%" PRIx32, uuid.uuid.uuid32);
      break;
    }
    case ESP_UUID_LEN_16: {
      sprintf(out, "%" PRIx16, uuid.uuid.uuid16);
      break;
    }
    default:
      break;
  }
};

/* Clone Utils */

/**
 * add_services
 *
 * Adds a service to our
 * clone struct.
 *
 * @param service_t* new_service
 */
bool add_service(const service_t *new_service) {
  if (clone.num_services == UINT8_MAX) return false;
  service_t *new_services = realloc(clone.services,
                                    (clone.num_services + 1) * sizeof(service_t));
  if (new_services == NULL) {
    return false;
  }

  clone.services = new_services;
  clone.services[clone.num_services] = *new_service;
  clone.services[clone.num_services].chars = NULL;
  clone.services[clone.num_services].num_chars = 0;
  clone.num_services++;
  return true;
}

/**
 * add_characteristic
 *
 * Adds a characteristic to a
 * service in our clone struct
 *
 * @param service_t* service
 * @param char_t* new_char
 */
bool add_characteristic(service_t *service, const char_t *new_char) {
  if (service->num_chars == UINT8_MAX) return false;
  char_t *new_chars = (char_t *)realloc(service->chars, (service->num_chars + 1) * sizeof(char_t));
  if (new_chars == NULL) {
    return false;
  }

  service->chars = new_chars;
  service->chars[service->num_chars] = *new_char;
  service->chars[service->num_chars].descs = NULL;
  service->chars[service->num_chars].num_descs = 0;
  service->num_chars++;
  return true;
}

/**
 * add_descriptor
 *
 * Adds a descriptor to a characteristic
 * in our clone struct
 *
 * @param char_t* character
 * @param desc_t* new_desc
 */
bool add_descriptor(char_t *character, const desc_t *new_desc) {
  if (character->num_descs == UINT8_MAX) return false;
  desc_t *new_descs = realloc(character->descs,
                             (character->num_descs + 1) * sizeof(desc_t));
  if (new_descs == NULL) {
    return false;
  }

  character->descs = new_descs;
  character->descs[character->num_descs] = *new_desc;
  character->num_descs++;
  return true;
}

/**
 * free_clone
 *
 * Free's our clone struct
 * and sets it to an init
 * state.
 */
void free_clone() {
  if (clone.services == NULL) return;

  for (size_t i = 0; i < clone.num_services; i++) {
    // Free all characteristics and their descriptors
    if (clone.services[i].chars != NULL) {
      for (size_t j = 0; j < clone.services[i].num_chars; j++) {
        free(clone.services[i].chars[j].value.attr_value);
        if (clone.services[i].chars[j].descs != NULL) {
          for (size_t k = 0; k < clone.services[i].chars[j].num_descs; ++k) {
            free(clone.services[i].chars[j].descs[k].value.attr_value);
          }
          free(clone.services[i].chars[j].descs);
        }
      }
      free(clone.services[i].chars);
    }
  }

  free(clone.services);

  clone.services = NULL;
  clone.num_services = 0;
}

/**
 * get_desc_parent_char_handle
 *
 * Returns the handle for the given
 * descriptor's parent characteristic->
 *
 * @param uint16_t handle - for desc
 * @return uint16_t handle - for parent char
 */
uint16_t get_desc_parent_char_handle(uint16_t desc_handle) {
  for (int x = 0; x < clone.num_services; x++) {
    for (int y = 0; y < clone.services[x].num_chars; y++) {
      for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
        if (clone.services[x].chars[y].descs[z].desc_handle == desc_handle) {
          return clone.services[x].chars[y].char_handle;
        }
      }
    }
  }

  return 0x01;
};

/**
 * handle_is_descriptor
 *
 * Determines if the input handle
 * is a descriptor or not using our
 * clone struct
 *
 * @param uint16_t handle
 * @return bool
 */
bool handle_is_descriptor(uint16_t handle) {
  for (int x = 0; x < clone.num_services; x++) {
    for (int y = 0; y < clone.services[x].num_chars; y++) {
      if (clone.services[x].chars[y].char_handle == handle) {
        return false;
      } else {
        for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
          if (clone.services[x].chars[y].descs[z].desc_handle == handle) {
            return true;
          }
        }
      }
    }
  }

  return false;
};

/**
 * get_uuid_for_handle
 *
 * Sets out variable to string
 * of the UUID give the handle
 *
 * @param uint16_t handle
 * @param char* out
 */
void get_uuid_for_handle(uint16_t handle, char* out) {
  for (int x = 0; x < clone.num_services; x++) {
    if (clone.services[x].service_handle == handle) {
      esp_bt_uuid_t uuid = clone.services[x].service_uuid;
      return print_uuid(uuid, out);
    }
    for (int y = 0; y < clone.services[x].num_chars; y++) {
      if (clone.services[x].chars[y].char_handle == handle) {
        esp_bt_uuid_t uuid = clone.services[x].chars[y].char_uuid;
        return print_uuid(uuid, out);
      } else {
        for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
          if (clone.services[x].chars[y].descs[z].desc_handle == handle) {
            esp_bt_uuid_t uuid = clone.services[x].chars[y].descs[z].desc_uuid;
            return print_uuid(uuid, out);
          }
        }
      }
    }
  }
};
