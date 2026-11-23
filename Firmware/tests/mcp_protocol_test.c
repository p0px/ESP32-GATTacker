#include "agent_api.h"
#include "mcp_protocol.h"
#include <assert.h>
#include <string.h>

static unsigned calls;
cJSON *agent_call(const char *name, const cJSON *args) {
  (void)args;
  calls++;
  cJSON *result = cJSON_CreateObject();
  cJSON_AddStringToObject(result, "tool", name);
  if (!strcmp(name, "network_status"))
    cJSON_AddStringToObject(result, "error", "ESP_ERR_TIMEOUT");
  return result;
}

static cJSON *request(const char *method, const char *name, const char *args) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "jsonrpc", "2.0");
  cJSON_AddStringToObject(root, "id", "request-123");
  cJSON_AddStringToObject(root, "method", method);
  cJSON *params = cJSON_AddObjectToObject(root, "params");
  cJSON *meta = cJSON_AddObjectToObject(params, "_meta");
  cJSON_AddStringToObject(meta, "io.modelcontextprotocol/protocolVersion", "2026-07-28");
  cJSON_AddObjectToObject(meta, "io.modelcontextprotocol/clientCapabilities");
  if (name)
    cJSON_AddStringToObject(params, "name", name);
  if (args)
    cJSON_AddItemToObject(params, "arguments", cJSON_Parse(args));
  return root;
}

static cJSON *send(const char *method, const char *name, const char *args) {
  cJSON *root = request(method, name, args);
  McpHeaders headers = {"2026-07-28", method, name ? name : ""};
  bool bad;
  cJSON *response = mcp_dispatch(root, &headers, &bad);
  assert(!bad);
  assert(!strcmp(cJSON_GetObjectItem(response, "id")->valuestring, "request-123"));
  cJSON_Delete(root);
  return response;
}

static void check_cache_metadata(const cJSON *result) {
  // Required by the 2026-07-28 wire schema, even for immediately stale results.
  const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(result, "ttlMs");
  const cJSON *scope = cJSON_GetObjectItemCaseSensitive(result, "cacheScope");
  assert(cJSON_IsNumber(ttl) && ttl->valuedouble >= 0);
  assert(cJSON_IsString(scope) && !strcmp(scope->valuestring, "private"));
}

int main(void) {
  cJSON *response = send("server/discover", NULL, NULL);
  cJSON *result = cJSON_GetObjectItem(response, "result");
  assert(!strcmp(cJSON_GetObjectItem(result, "resultType")->valuestring, "complete"));
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(result, "supportedVersions")) == 2);
  check_cache_metadata(result);
  cJSON_Delete(response);
  response = send("tools/list", NULL, NULL);
  check_cache_metadata(cJSON_GetObjectItem(response, "result"));
  assert(cJSON_GetArraySize(
             cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "tools")) >= 10);
  cJSON_Delete(response);
  response = send("tools/call", "session_start", "{\"mac\":\"EF:FA:F8:19:DA:67\"}");
  assert(calls == 1);
  assert(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "isError")));
  cJSON_Delete(response);
  const char *invalid[] = {"{}", "{\"mac\":42}", "{\"mac\":\"short\"}",
                           "{\"mac\":\"EF:FA:F8:19:DA:67\",\"extra\":true}"};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    response = send("tools/call", "session_start", invalid[i]);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "isError")));
    assert(calls == 1);
    cJSON_Delete(response);
  }
  response = send("tools/call", "not_a_tool", "{}");
  assert(cJSON_GetObjectItem(response, "error"));
  cJSON_Delete(response);
  cJSON *root = request("tools/call", "session_stop", "{}");
  McpHeaders headers = {"2026-07-28", "tools/call", "session_start"};
  bool bad;
  response = mcp_dispatch(root, &headers, &bad);
  assert(bad && calls == 1);
  assert(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "code")->valueint == -32020);
  cJSON_Delete(response);
  headers.version = "2099-01-01";
  response = mcp_dispatch(root, &headers, &bad);
  assert(bad && cJSON_GetObjectItem(response, "error"));
  cJSON_Delete(response);
  cJSON_Delete(root);
  response = send("tools/call", "network_status", "{}");
  result = cJSON_GetObjectItem(response, "result");
  assert(!cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
  assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(result, "structuredContent"),
                                    "error")->valuestring, "ESP_ERR_TIMEOUT"));
  cJSON_Delete(response);
  response = mcp_dispatch(NULL, &headers, &bad);
  assert(bad && cJSON_GetObjectItem(response, "error"));
  cJSON_Delete(response);
}
