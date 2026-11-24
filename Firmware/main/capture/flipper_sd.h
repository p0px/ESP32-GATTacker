#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool connected;
  bool available;
  bool complete;
  uint32_t id;
  uint32_t packets;
  uint32_t dropped;
  uint32_t bytes;
  char state[12];
  char path[128];
  char error[96];
} FlipperSdStatus;

void flipper_sd_init(void);
FlipperSdStatus flipper_sd_status(void);
bool flipper_sd_receive(char **fields, size_t count);
const char *flipper_sd_request(bool start, uint32_t id);
