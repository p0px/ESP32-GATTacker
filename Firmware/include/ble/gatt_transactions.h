#pragma once

#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  AttNone,
  AttRead,
  AttPrepare,
  AttExecute,
} AttOperation;

typedef struct {
  AttOperation operation;
  uint16_t central;
  uint16_t target;
  uint32_t transaction;
  uint16_t handle;
  uint16_t offset;
  bool descriptor;
  bool abandoned;
  uint16_t central_handle;
} AttRequest;

/* Bluedroid's GATT callback task owns these requests until teardown quiesces it. */
typedef struct {
  AttRequest request;
  esp_gatt_rsp_t prepared;
  uint16_t central_mtu;
  uint16_t prepared_owner;
  bool queued;
  bool canceling;
} GattTransactions;

typedef struct GattQueuedWrite {
  struct GattQueuedWrite *next;
  uint16_t central;
  uint16_t handle;
  uint16_t length;
  bool descriptor;
  bool response;
  uint8_t value[];
} GattQueuedWrite;

typedef struct {
  GattQueuedWrite *head;
  GattQueuedWrite *tail;
  size_t bytes;
  unsigned count;
  unsigned peak;
  bool in_flight;
  bool congested;
  bool pacing_requested;
} GattWriteQueue;
