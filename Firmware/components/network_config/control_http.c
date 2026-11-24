#include "control_http.h"
#include "lwip/sockets.h"
#include "network_service.h"
#include "network_pair.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool address(httpd_req_t *req, char out[16]) {
  struct sockaddr_storage local;
  socklen_t size = sizeof(local);
  if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) != 0)
    return false;
  if (local.ss_family == AF_INET)
    return inet_ntop(AF_INET, &((struct sockaddr_in *)&local)->sin_addr, out, 16) != NULL;
  // ESP-IDF's dual-stack listener reports IPv4 peers as IPv4-mapped IPv6 addresses.
  if (local.ss_family == AF_INET6) {
    const struct in6_addr *ip = &((struct sockaddr_in6 *)&local)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(ip))
      return inet_ntop(AF_INET, &ip->s6_addr[12], out, 16) != NULL;
  }
  return false;
}

bool control_origin(httpd_req_t *req) {
  char origin[64], ip[16], host[32], expected[48];
  if (!address(req, ip) || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
    return false;
  snprintf(expected, sizeof(expected), "%s:80", ip);
  if (strcmp(host, ip) && strcmp(host, expected))
    return false;
  if (!httpd_req_get_hdr_value_len(req, "Origin"))
    return true;
  if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK)
    return false;
  snprintf(expected, sizeof(expected), "http://%s", ip);
  if (!strcmp(origin, expected))
    return true;
  strcat(expected, ":80");
  return !strcmp(origin, expected);
}

bool control_setup_ap_request(httpd_req_t *req) {
  char ip[16];
  return address(req, ip) && !strcmp(ip, "1.3.3.7");
}

bool control_access(httpd_req_t *req, bool allow_setup_ap) {
  if (!control_origin(req))
    return false;
  char bearer[48];
  if (allow_setup_ap && control_setup_ap_request(req))
    return true;
  return httpd_req_get_hdr_value_str(req, "Authorization", bearer, sizeof(bearer)) == ESP_OK &&
         !strncmp(bearer, "Bearer ", 7) && network_authorized(bearer + 7);
}

bool control_settings_access(httpd_req_t *req) {
  if (control_access(req, true)) return true;
  if (!control_origin(req)) return false;
  char bearer[48];
  return httpd_req_get_hdr_value_str(req, "Authorization", bearer, sizeof(bearer)) == ESP_OK &&
         !strncmp(bearer, "Bearer ", 7) && network_pair_authorized(req, bearer + 7);
}

esp_err_t control_json(httpd_req_t *req, cJSON *json) {
  char *body = json ? cJSON_PrintUnformatted(json) : NULL;
  cJSON_Delete(json);
  if (!body)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  free(body);
  return result;
}

esp_err_t control_error(httpd_req_t *req, const char *status, const char *message) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_hdr(req, "Connection", "close");
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "error", message);
  return control_json(req, json);
}

cJSON *control_read(httpd_req_t *req) {
  if (!req->content_len || req->content_len > 2048)
    return NULL;
  char type[64];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
      (strcmp(type, "application/json") && strcmp(type, "application/json; charset=utf-8")))
    return NULL;
  char *body = calloc(req->content_len + 1, 1);
  if (!body)
    return NULL;
  size_t used = 0;
  while (used < req->content_len) {
    int count = httpd_req_recv(req, body + used, req->content_len - used);
    if (count <= 0) {
      free(body);
      return NULL;
    }
    used += count;
  }
  // Bound cJSON recursion on the HTTP task's small stack, including unknown parameters.
  unsigned depth = 0;
  bool quoted = false, escaped = false, valid = !memchr(body, 0, used);
  for (size_t i = 0; i < used && valid; i++) {
    char ch = body[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quoted && ch == '\\') {
      // cJSON strings cannot represent embedded NULs; reject instead of truncating credentials.
      if (i + 5 < used && !memcmp(body + i + 1, "u0000", 5))
        valid = false;
      escaped = true;
      continue;
    }
    if (ch == '"')
      quoted = !quoted;
    if (!quoted && (ch == '{' || ch == '[') && ++depth > 12)
      valid = false;
    if (!quoted && (ch == '}' || ch == ']') && depth)
      depth--;
  }
  cJSON *json = valid ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
  memset(body, 0, used);
  free(body);
  return json;
}
