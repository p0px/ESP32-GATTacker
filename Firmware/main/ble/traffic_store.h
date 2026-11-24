#ifndef GATT_TRAFFIC_STORE_H
#define GATT_TRAFFIC_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define TRAFFIC_VALUE_MAX 1034

typedef struct {
  uint32_t cursor;
  uint32_t time_ms;
  uint32_t transaction;
  uint32_t status;
  uint16_t handle;
  bool has_data;
  bool truncated;
  char operation[24];
  char direction[24];
  char kind[8];
  char uuid[38];
  char data[TRAFFIC_VALUE_MAX + 1];
} TrafficEvent;

typedef struct {
  uint32_t oldest;
  uint32_t newest;
  uint32_t capacity;
  uint32_t uptime_ms;
} TrafficPosition;

void traffic_store_init(void);
void traffic_store_record(const char *operation, const char *direction, const char *kind,
                          uint32_t transaction, uint16_t handle, uint32_t status,
                          const char *uuid, const char *data);
TrafficPosition traffic_store_position(void);
bool traffic_store_read(uint32_t cursor, TrafficEvent *event);

#endif
