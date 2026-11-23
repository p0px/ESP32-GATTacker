#pragma once
#include "cJSON.h"
#include <stdbool.h>
typedef struct {
  const char *version;
  const char *method;
  const char *name;
} McpHeaders;
cJSON *mcp_dispatch(const cJSON *request, const McpHeaders *headers, bool *bad_request);
