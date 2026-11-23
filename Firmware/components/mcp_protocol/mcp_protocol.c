#include "mcp_protocol.h"
#include "agent_api.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *value(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static cJSON *failure(const cJSON *id, int code, const char *message) {
  cJSON *reply = cJSON_CreateObject();
  cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
  cJSON_AddItemToObject(reply, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
  cJSON *error = cJSON_AddObjectToObject(reply, "error");
  cJSON_AddNumberToObject(error, "code", code);
  cJSON_AddStringToObject(error, "message", message);
  if (code == -32022) {
    cJSON *data = cJSON_AddObjectToObject(error, "data");
    cJSON *versions = cJSON_AddArrayToObject(data, "supported");
    cJSON_AddItemToArray(versions, cJSON_CreateString("2026-07-28"));
    cJSON_AddItemToArray(versions, cJSON_CreateString("2025-11-25"));
  }
  return reply;
}

static cJSON *identity(void) {
  cJSON *info = cJSON_CreateObject();
  cJSON_AddStringToObject(info, "name", "esp32-gattacker");
  cJSON_AddStringToObject(info, "version", "1.0.0");
  return info;
}

static bool arguments_valid(const cJSON *schema, const cJSON *args) {
  if (args && !cJSON_IsObject(args))
    return false;
  const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required"), *field;
  cJSON_ArrayForEach(field, required) {
    if (!cJSON_HasObjectItem(args, field->valuestring))
      return false;
  }
  const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
  cJSON_ArrayForEach(field, args) {
    const cJSON *rule = cJSON_GetObjectItemCaseSensitive(properties, field->string);
    if (!rule)
      return false;
    if (!strcmp(value(rule, "type"), "integer")) {
      const cJSON *min = cJSON_GetObjectItemCaseSensitive(rule, "minimum");
      const cJSON *max = cJSON_GetObjectItemCaseSensitive(rule, "maximum");
      if (!cJSON_IsNumber(field) || floor(field->valuedouble) != field->valuedouble ||
          (min && field->valuedouble < min->valuedouble) ||
          (max && field->valuedouble > max->valuedouble))
        return false;
    } else {
      if (!cJSON_IsString(field))
        return false;
      const cJSON *min = cJSON_GetObjectItemCaseSensitive(rule, "minLength");
      const cJSON *max = cJSON_GetObjectItemCaseSensitive(rule, "maxLength");
      size_t length = strlen(field->valuestring);
      if ((min && length < min->valuedouble) || (max && length > max->valuedouble))
        return false;
    }
  }
  return true;
}

static cJSON *call(const cJSON *id, const cJSON *params) {
  const char *name = value(params, "name");
  const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
  cJSON *catalog = agent_catalog(), *tool;
  bool found = false, valid = false;
  cJSON_ArrayForEach(tool, catalog) {
    if (strcmp(value(tool, "name"), name))
      continue;
    found = true;
    valid = arguments_valid(cJSON_GetObjectItemCaseSensitive(tool, "inputSchema"), args);
    break;
  }
  cJSON_Delete(catalog);
  if (!found)
    return failure(id, -32602, "Unknown tool");
  cJSON *data = NULL;
  if (valid)
    data = agent_call(name, args);
  else {
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "error", "Invalid tool arguments");
  }
  if (!data)
    return failure(id, -32603, "Out of memory");
  cJSON *result = cJSON_CreateObject();
  const cJSON *error = cJSON_GetObjectItemCaseSensitive(data, "error");
  // Status snapshots include an error field too; only an error-only envelope fails the tool.
  bool failed = cJSON_GetArraySize(data) == 1 && cJSON_IsString(error) && error->valuestring[0];
  cJSON_AddBoolToObject(result, "isError", failed);
  cJSON *content = cJSON_AddArrayToObject(result, "content"), *text = cJSON_CreateObject();
  char *serialized = cJSON_PrintUnformatted(data);
  cJSON_AddStringToObject(text, "type", "text");
  cJSON_AddStringToObject(text, "text", serialized ? serialized : "Out of memory");
  free(serialized);
  cJSON_AddItemToArray(content, text);
  cJSON_AddItemToObject(result, "structuredContent", data);
  return result;
}

cJSON *mcp_dispatch(const cJSON *request, const McpHeaders *headers, bool *bad_request) {
  *bad_request = false;
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
  const char *method = value(request, "method");
  if (!request) {
    *bad_request = true;
    return failure(NULL, -32700, "Invalid JSON");
  }
  if (!cJSON_IsObject(request) || strcmp(value(request, "jsonrpc"), "2.0") || !*method ||
      (params && !cJSON_IsObject(params)) || (!cJSON_IsString(id) && !cJSON_IsNumber(id))) {
    *bad_request = true;
    return failure(NULL, -32600, "Expected one JSON-RPC request with an id");
  }
  bool initialize = !strcmp(method, "initialize");
  bool modern = !strcmp(headers->version, "2026-07-28");
  if (!initialize && !modern && strcmp(headers->version, "2025-11-25")) {
    *bad_request = true;
    return failure(id, -32022, "Unsupported protocol version");
  }
  if (modern) {
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    if (strcmp(value(meta, "io.modelcontextprotocol/protocolVersion"), headers->version) ||
        strcmp(method, headers->method) ||
        (!strcmp(method, "tools/call") && strcmp(value(params, "name"), headers->name))) {
      *bad_request = true;
      return failure(id, -32020, "Required MCP headers do not match the request");
    }
    if (!cJSON_IsObject(
            cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/clientCapabilities"))) {
      *bad_request = true;
      return failure(id, -32602, "clientCapabilities metadata is required");
    }
  }
  cJSON *result = NULL;
  if (initialize || !strcmp(method, "server/discover")) {
    result = cJSON_CreateObject();
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    if (initialize) {
      cJSON_AddStringToObject(result, "protocolVersion", "2025-11-25");
      cJSON_AddItemToObject(result, "serverInfo", identity());
    } else {
      cJSON *versions = cJSON_AddArrayToObject(result, "supportedVersions");
      cJSON_AddItemToArray(versions, cJSON_CreateString("2026-07-28"));
      cJSON_AddItemToArray(versions, cJSON_CreateString("2025-11-25"));
    }
    cJSON_AddStringToObject(
        result, "instructions",
        "Use explicit target MACs. Poll operation_status and board_status for completion. "
        "Other controllers share this session. Preview is sampled; download PCAP for packets.");
  } else if (!strcmp(method, "ping")) {
    result = cJSON_CreateObject();
  } else if (!strcmp(method, "tools/list")) {
    if (cJSON_HasObjectItem(params, "cursor"))
      return failure(id, -32602, "Invalid cursor");
    result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", agent_catalog());
  } else if (!strcmp(method, "tools/call")) {
    result = call(id, params);
    if (cJSON_HasObjectItem(result, "jsonrpc"))
      return result;
  } else
    return failure(id, -32601, "Method not supported");
  if (!result)
    return failure(id, -32603, "Out of memory");
  if (modern) {
    cJSON_AddStringToObject(result, "resultType", "complete");
    if (!strcmp(method, "server/discover") || !strcmp(method, "tools/list")) {
      cJSON_AddNumberToObject(result, "ttlMs", 0);
      cJSON_AddStringToObject(result, "cacheScope", "private");
    }
    cJSON *meta = cJSON_AddObjectToObject(result, "_meta");
    cJSON_AddItemToObject(meta, "io.modelcontextprotocol/serverInfo", identity());
  }
  cJSON *reply = cJSON_CreateObject();
  cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
  cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, true));
  cJSON_AddItemToObject(reply, "result", result);
  return reply;
}
