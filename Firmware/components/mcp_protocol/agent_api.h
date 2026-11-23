#pragma once
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
// The MCP worker owns params until this serialized command completes.
void agent_init(void);
cJSON *agent_call(const char *name, const cJSON *arguments);
cJSON *agent_catalog(void);
#ifdef __cplusplus
}
#endif
