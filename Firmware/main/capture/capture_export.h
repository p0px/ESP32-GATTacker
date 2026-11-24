#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool active;
  size_t bytes;
  uint32_t id;
  char state[16];
} CaptureExportStatus;

void capture_export_init(void);
bool capture_export_receive(char **fields, size_t count);
const char *capture_export_start(uint32_t id);
CaptureExportStatus capture_export_status(void);
