#ifndef GATT_INTERNAL_H
#define GATT_INTERNAL_H

#include "ble/GattAttack.h"
#include "ble/GattAttackApp.h"
#include "ble/ble.h"
#include "ble/payload.h"
#include "uart/uart_bridge.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "GATT Attack"

// A write response belongs to the original phone request, not the most recent
// read or write-without-response seen on either connection.
typedef struct pending_write_t {
  bool active;
  bool descriptor;
  uint16_t conn_id;
  uint32_t trans_id;
  uint16_t handle;
  int64_t started_us;
} pending_write_t;

extern GattAttackState gattAttackState;
extern atomic_bool shutdown_gatt_attack;
extern pending_write_t pending_write;
extern int retries;
extern int MAX_RETRIES;
extern bool connected;
extern bool connecting;
extern bool clone_init;
extern uint8_t service_num;
extern uint8_t cloned_service_num;
extern uint8_t last_end_handle;

void reply_cached_read(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void finish_pending_write(esp_gatt_status_t status);
void log_and_send_json(const char *operation, const char *direction, const char *kind,
                       uint32_t interface, uint32_t connection, uint32_t transaction,
                       uint16_t handle, uint32_t status, const char *uuid,
                       const char *extra_data);
uint8_t *hex_string_to_bytes(const char *hex_string, uint16_t *out_len);
void apply_hook(const char *type, const char *uuid, char **hex,
                uint8_t **bytes, uint16_t *length);
bool add_service(const service_t *new_service);
bool add_characteristic(service_t *service, const char_t *new_char);
bool add_descriptor(char_t *character, const desc_t *new_desc);
void free_clone(void);
uint16_t get_desc_parent_char_handle(uint16_t desc_handle);
bool handle_is_descriptor(uint16_t handle);
void get_uuid_for_handle(uint16_t handle, char *out);

bool att_busy(void);
esp_gatt_status_t submission_status(esp_err_t result);
void att_reply(AttRequest request, esp_gatt_status_t status, esp_gatt_rsp_t *response);
void log_att(AttRequest request, const char *operation, esp_gatt_status_t status,
             const uint8_t *data, size_t length, bool response);
void start_read(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void finish_read(bool descriptor, esp_ble_gattc_cb_param_t *param);
void start_prepare(esp_ble_gatts_cb_param_t *param);
void finish_prepare(esp_ble_gattc_cb_param_t *param);
void start_execute(esp_ble_gatts_cb_param_t *param);
void finish_execute(esp_ble_gattc_cb_param_t *param);
void central_transactions_disconnected(uint16_t connection);
void target_transactions_disconnected(void);

void clone_failed(const char *operation, int status);
void clone_create_services(void);
void clone_register(esp_gatt_if_t interface, esp_ble_gatts_cb_param_t *param);
void clone_created(esp_ble_gatts_cb_param_t *param);
void clone_started(esp_ble_gatts_cb_param_t *param);
void clone_verify_service(esp_gatt_if_t interface, esp_ble_gattc_cb_param_t *param);
void clone_service_deleted(esp_ble_gatts_cb_param_t *param);
void clone_verify_complete(void);

bool write_queue_add(uint16_t central, uint16_t handle, bool descriptor,
                     bool response, const uint8_t *value, uint16_t length);
void write_queue_reset(void);
void write_queue_target_disconnected(void);
void write_queue_central_disconnected(uint16_t central);
void write_queue_complete(bool descriptor, esp_ble_gattc_cb_param_t *param);
void write_queue_resume(uint16_t target, bool congested);

void connection_failed(const char *operation);
void server_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                  esp_ble_gatts_cb_param_t *param);
void client_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                  esp_ble_gattc_cb_param_t *param);
void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

#endif
