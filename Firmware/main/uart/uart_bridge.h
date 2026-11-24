#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Flipper UART transport shares the web UI's session controller. */
void uart_bridge_start(void);
void uart_bridge_state(void);
void uart_bridge_target(const char *mac);
void uart_bridge_phone(bool connected);
void uart_bridge_event(const char *event);
void uart_bridge_traffic(const char *operation, const char *direction,
                         const char *uuid, uint16_t handle, const char *hex);
char *uart_bridge_hook(const char *type, const char *uuid, const char *hex);
bool uart_bridge_rules_active(void);
bool uart_bridge_connected(void);
