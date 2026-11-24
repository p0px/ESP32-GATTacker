#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool capture_wire_receive(char **fields, size_t count);
bool capture_wire_command(uint16_t id, char **fields, size_t count);
void capture_wire_status(void);
bool capture_wire_poll(char *out, size_t size);
