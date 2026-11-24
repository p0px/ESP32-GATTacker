#pragma once

#include "wire.h"
#include <stdbool.h>
#include <stdint.h>

void uart_emit(WireKind kind, uint16_t id, const char *text);
bool uart_emit_wait(WireKind kind, uint16_t id, const char *text, uint32_t wait_ms);
void uart_command_execute(uint16_t id, const char *text);
void uart_command_state(void);
void uart_command_target(const char *mac);
void uart_command_phone(bool connected);
void uart_command_init(void);
bool uart_network_command(uint16_t id, char **fields, size_t count);
uint32_t uart_dropped(void);
