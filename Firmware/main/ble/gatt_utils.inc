// Included in GattAttack.c to share callback state within one C translation unit.
/* Utils */

/**
 * log_and_send_json
 *
 * Formats messages as JSON and sends via WS.
 *
 * @param char* operation - eg. "WRITE", "READ"
 * @param char* direction - eg. "victim -> target", "cache -> victim"
 * @param char* type - eg. "CHAR", "DESC"
 * @param uint32_t gatt_if - Gatt Interface
 * @param uint32_t conn_id - Connection ID
 * @param uint32_t trans_id - Transaction ID
 * @param uint16_t handle - Connection Handle
 * @param uint32_t status - Status of call
 * @param char* uuid
 * @param char* extra_data - Extra data as string (hex output)
 */
static void log_and_send_json(const char *operation, // e.g., "WRITE", "READ"
                              const char *direction, // e.g., "victim -> target", "cache -> victim"
                              const char *type,      // e.g., "CHAR", "DESC"
                              uint32_t gatt_if, uint32_t conn_id, uint32_t trans_id,
                              uint16_t handle, uint32_t status, const char *uuid,
                              const char *extra_data) {
  uart_bridge_traffic(operation, direction, uuid, handle, extra_data);
  if (!ws_has_clients()) return;
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "operation", operation);
  cJSON_AddStringToObject(json, "direction", direction);
  cJSON_AddStringToObject(json, "type", type);
  cJSON_AddNumberToObject(json, "gatt_if", gatt_if);
  cJSON_AddNumberToObject(json, "conn_id", conn_id);
  cJSON_AddNumberToObject(json, "trans_id", trans_id);
  cJSON_AddNumberToObject(json, "handle", handle);
  cJSON_AddNumberToObject(json, "status", status);
  cJSON_AddStringToObject(json, "uuid", uuid);

  if (extra_data) {
    cJSON_AddStringToObject(json, "data", extra_data);
  }

  char *json_string = cJSON_Print(json);
  if (json_string) {
    gatt_send_output(json_string, true);
    free(json_string);
  }

  cJSON_Delete(json);
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
char *bytes_to_hex_string(const uint8_t *data, size_t data_len) {
  // Calculate necessary length for the hex string (2 chars per byte + null terminator)
  size_t hex_string_size = (data_len * 2) + 1;
  char *hex_string = (char *)malloc(hex_string_size);

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

uint8_t *hex_string_to_bytes(const char *hex_string, uint16_t *out_len) {
  return decode_hex_payload(hex_string, out_len, ESP_GATT_MAX_ATTR_LEN);
}

static void apply_hook(const char *type, const char *uuid, char **hex, uint8_t **bytes,
                       uint16_t *length) {
  if (!*hex)
    return;
  char *reply = uart_bridge_hook(type, uuid, *hex);
  if (!reply && should_run_hooks()) reply = run_hook(type, uuid, *hex);
  if (!reply)
    return;
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
void print_uuid(esp_bt_uuid_t uuid, char *out) {
  switch (uuid.len) {
  case ESP_UUID_LEN_128: {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid.uuid.uuid128[15], uuid.uuid.uuid128[14], uuid.uuid.uuid128[13],
             uuid.uuid.uuid128[12], uuid.uuid.uuid128[11], uuid.uuid.uuid128[10],
             uuid.uuid.uuid128[9], uuid.uuid.uuid128[8], uuid.uuid.uuid128[7], uuid.uuid.uuid128[6],
             uuid.uuid.uuid128[5], uuid.uuid.uuid128[4], uuid.uuid.uuid128[3], uuid.uuid.uuid128[2],
             uuid.uuid.uuid128[1], uuid.uuid.uuid128[0]);
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
  if (clone.num_services == UINT8_MAX)
    return false;
  service_t *new_services =
      (service_t *)realloc(clone.services, (clone.num_services + 1) * sizeof(service_t));
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
  if (service->num_chars == UINT8_MAX)
    return false;
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
  if (character->num_descs == UINT8_MAX)
    return false;
  desc_t *new_descs =
      (desc_t *)realloc(character->descs, (character->num_descs + 1) * sizeof(desc_t));
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
void free_clone(void) {
  if (clone.services == NULL)
    return;

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
 * descriptor's parent characteristic.
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
void get_uuid_for_handle(uint16_t handle, char *out) {
  for (int x = 0; x < clone.num_services; x++) {
    if (clone.services[x].service_handle == handle) {
      esp_bt_uuid_t uuid = clone.services[x].service_uuid;
      print_uuid(uuid, out);
      return;
    }
    for (int y = 0; y < clone.services[x].num_chars; y++) {
      if (clone.services[x].chars[y].char_handle == handle) {
        esp_bt_uuid_t uuid = clone.services[x].chars[y].char_uuid;
        print_uuid(uuid, out);
        return;
      } else {
        for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
          if (clone.services[x].chars[y].descs[z].desc_handle == handle) {
            esp_bt_uuid_t uuid = clone.services[x].chars[y].descs[z].desc_uuid;
            print_uuid(uuid, out);
            return;
          }
        }
      }
    }
  }
};

/**
 * prep_write_event_env
 *
 * Handles a PREP Write to
 * either a char or desc. It
 * will copy the bytes into
 * our prepared_write_env
 * variable. Then it will
 * send our attacking client
 * to send the same write.
 *
 * Called from ESP_GATTS_WRITE_EVT
 * when it is a prepped write.
 *
 * @param esp_ble_gatts_cb_param_t *param
 */
void prep_write_event_env(esp_ble_gatts_cb_param_t *param) {
  if (param->write.len > ESP_GATT_MAX_ATTR_LEN || param->write.offset > 2048 ||
      param->write.len > 2048 - param->write.offset) {
    esp_ble_gatts_send_response(clone.gatts_if, param->write.conn_id, param->write.trans_id,
                                ESP_GATT_INVALID_ATTR_LEN, NULL);
    return;
  }

  char uuid[38] = {0};
  bool isDescriptor = handle_is_descriptor(param->write.handle);
  get_uuid_for_handle(param->write.handle, uuid);
  esp_gatt_status_t status = ESP_GATT_OK;

  char *value = bytes_to_hex_string(param->write.value, param->write.len);

  log_and_send_json("PREP WRITE", connected ? "victim -> target" : "victim -> cache",
                    isDescriptor ? "DESC" : "CHAR", clone.gattc_if, clone.target_client_conn_id,
                    param->write.trans_id, param->write.handle, 0, uuid, value);

  free(value);

  if (connected) {
    if (!isDescriptor) {
      esp_ble_gattc_prepare_write(clone.gattc_if, clone.target_client_conn_id, param->write.handle,
                                  param->write.offset, param->write.len, param->write.value,
                                  ESP_GATT_AUTH_REQ_NONE);
    } else {
      esp_ble_gattc_prepare_write_char_descr(
          clone.gattc_if, clone.target_client_conn_id, param->write.handle, param->write.offset,
          param->write.len, param->write.value, ESP_GATT_AUTH_REQ_NONE);
    }
  }

  if (prepare_write_env.prepare_buf == NULL) {
    prepare_write_env.prepare_buf = (uint8_t *)calloc(2048, sizeof(uint8_t));
    prepare_write_env.prepare_len = 0;
    if (prepare_write_env.prepare_buf == NULL) {
      ESP_LOGE(TAG, "Gatt_server prep no mem");
      esp_ble_gatts_send_response(clone.gatts_if, param->write.conn_id, param->write.trans_id,
                                  ESP_GATT_NO_RESOURCES, NULL);
      return;
    }
  }

  esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
  if (gatt_rsp) {
    gatt_rsp->attr_value.len = param->write.len;
    gatt_rsp->attr_value.handle = param->write.handle;
    gatt_rsp->attr_value.offset = param->write.offset;
    gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
    memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
    esp_ble_gatts_send_response(clone.gatts_if, param->write.conn_id, param->write.trans_id, status,
                                gatt_rsp);
    free(gatt_rsp);
  }

  /**
   * Im not quite sure if offset is 0
   * we need to overwrite or append to
   * our prepare_buf. Right now I am
   * going to just overwrite it.
   *
   * Using nRF Connect I have been unable
   * to see the Reliable Write ever do
   * anything other than offset 0.
   */
  if (param->write.offset + param->write.len <= 2048) {
    memcpy(prepare_write_env.prepare_buf + param->write.offset, param->write.value,
           param->write.len);
  }

  const int end = param->write.offset + param->write.len;
  if (param->write.offset == 0 || end > prepare_write_env.prepare_len) {
    prepare_write_env.prepare_len = end;
  }
}

