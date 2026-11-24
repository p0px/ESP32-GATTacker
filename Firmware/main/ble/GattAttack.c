#include "gatt_internal.h"

clone_t clone;
esp_ble_scan_params_t ble_scan_params = {
  .scan_type = BLE_SCAN_TYPE_ACTIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
  .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL, .scan_interval = 0x50,
  .scan_window = 0x30, .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};
esp_ble_adv_params_t adv_params = {
  .adv_int_min = 0x20, .adv_int_max = 0x20, .adv_type = ADV_TYPE_IND,
  .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .channel_map = ADV_CHNL_ALL,
  .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};
