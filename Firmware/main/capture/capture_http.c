#include "capture/capture_http.h"
#include "capture/capture_store.h"
#include "control_http.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart/uart_bridge.h"
#include "wire.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_number(cJSON *json, const char *key, double value) {
  cJSON_AddNumberToObject(json, key, value);
}

static esp_err_t capture_status_http(httpd_req_t *request) {
  CaptureStoreStatus state = capture_store_status();
  const char *names[] = {"empty", "recording", "saving", "ready"};
  cJSON *json = cJSON_CreateObject();
  if (!json) return control_error(request, "503 Service Unavailable", "Out of memory");
  cJSON_AddBoolToObject(json, "available", state.available);
  cJSON_AddStringToObject(json, "owner", state.flipper ? "flipper" :
                          state.state == CaptureRecording || state.state == CaptureSaving ?
                          "web" : "none");
  cJSON_AddStringToObject(json, "state", names[state.state]);
  add_number(json, "id", state.id);
  add_number(json, "packets", state.packets);
  add_number(json, "dropped", state.dropped);
  add_number(json, "full", state.full);
  add_number(json, "invalid", state.invalid);
  add_number(json, "peak", state.peak);
  add_number(json, "bytes", state.bytes);
  add_number(json, "capacity", state.capacity);
  cJSON_AddBoolToObject(json, "downloading", state.downloading);
  cJSON_AddBoolToObject(json, "auto_export", state.auto_export);
  cJSON_AddBoolToObject(json, "export_failed", state.export_failed);
  cJSON_AddStringToObject(json, "reason", state.reason);
  cJSON *sd = cJSON_AddObjectToObject(json, "sd");
  cJSON_AddBoolToObject(sd, "connected", uart_bridge_connected());
  cJSON_AddBoolToObject(sd, "available", false);
  cJSON_AddStringToObject(sd, "state", "idle");
  add_number(sd, "id", 0);
  add_number(sd, "packets", 0);
  add_number(sd, "dropped", 0);
  add_number(sd, "bytes", 0);
  cJSON_AddBoolToObject(sd, "complete", false);
  cJSON_AddStringToObject(sd, "path", "");
  cJSON_AddStringToObject(sd, "error", "");
  cJSON *export = cJSON_AddObjectToObject(json, "export");
  cJSON_AddStringToObject(export, "state", "idle");
  add_number(export, "bytes", 0);
  add_number(export, "id", 0);
  return control_json(request, json);
}

static bool read_number(httpd_req_t *request, const char *key, uint32_t *out,
                        char destination[16]) {
  cJSON *json = control_read(request);
  const cJSON *number = json ? cJSON_GetObjectItemCaseSensitive(json, key) : NULL;
  const cJSON *where = json ? cJSON_GetObjectItemCaseSensitive(json, "destination") : NULL;
  bool valid = cJSON_IsObject(json) && cJSON_IsNumber(number) &&
               number->valuedouble >= 1 && number->valuedouble <= UINT32_MAX &&
               floor(number->valuedouble) == number->valuedouble;
  if (valid && where)
    valid = cJSON_IsString(where) && strlen(where->valuestring) < 16;
  if (valid) {
    *out = (uint32_t)number->valuedouble;
    snprintf(destination, 16, "%s", where ? where->valuestring : "web");
    valid = !strcmp(destination, "web") || !strcmp(destination, "flipper") ||
            !strcmp(destination, "flipper_live");
  }
  cJSON_Delete(json);
  return valid;
}

static esp_err_t capture_control_http(httpd_req_t *request) {
  if (strchr(request->uri, '?'))
    return control_error(request, "400 Bad Request", "Capture controls reject query strings");
  bool start = !strcmp(request->uri, "/api/capture/start");
  uint32_t value;
  char destination[16];
  if (!read_number(request, start ? "seconds" : "id", &value, destination))
    return control_error(request, "400 Bad Request", "Expected a positive uint32 value");
  if (strcmp(destination, "web"))
    return control_error(request, "409 Conflict", "Flipper capture is unavailable");
  const char *error = start ? capture_store_start(value, false, false) :
                      !strcmp(request->uri, "/api/capture/stop") ?
                      capture_store_stop(value) : capture_store_clear(value);
  if (error) return control_error(request, "409 Conflict", error);
  return capture_status_http(request);
}

typedef struct {
  httpd_req_t *request;
  CaptureDownload download;
} DownloadJob;

static void download_task(void *context) {
  DownloadJob *job = context;
  char disposition[100];
  snprintf(disposition, sizeof(disposition), "attachment; filename=esp32-capture-%lu.pcapng",
           (unsigned long)job->download.id);
  httpd_resp_set_type(job->request, "application/x-pcapng");
  httpd_resp_set_hdr(job->request, "Content-Disposition", disposition);
  httpd_resp_set_hdr(job->request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(job->request, "X-Content-Type-Options", "nosniff");
  bool sent = true;
  for (size_t offset = 0; offset < job->download.size; offset += 4096) {
    size_t length = job->download.size - offset;
    if (length > 4096) length = 4096;
    if (httpd_resp_send_chunk(job->request,
                             (const char *)job->download.bytes + offset, length) != ESP_OK) {
      sent = false;
      break;
    }
  }
  if (sent) httpd_resp_send_chunk(job->request, NULL, 0);
  capture_store_release();
  httpd_req_async_handler_complete(job->request);
  free(job);
  vTaskDelete(NULL);
}

static esp_err_t capture_download_http(httpd_req_t *request) {
  char query[64], token[16];
  uint32_t id;
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", token, sizeof(token)) != ESP_OK ||
      !wire_number(token, UINT32_MAX, &id) || !id)
    return control_error(request, "400 Bad Request", "Expected capture id in the query");
  DownloadJob *job = calloc(1, sizeof(*job));
  if (!job) return control_error(request, "503 Service Unavailable", "Out of memory");
  if (!capture_store_download(&job->download)) {
    free(job);
    return control_error(request, "409 Conflict", "Capture is still recording or downloading");
  }
  if (job->download.id != id) {
    capture_store_release();
    free(job);
    return control_error(request, "409 Conflict", "Capture changed; refresh status");
  }
  if (httpd_req_async_handler_begin(request, &job->request) != ESP_OK) {
    capture_store_release();
    free(job);
    return control_error(request, "503 Service Unavailable", "Download worker unavailable");
  }
  if (xTaskCreate(download_task, "pcap_download", 4096, job, 2, NULL) != pdPASS) {
    control_error(job->request, "503 Service Unavailable", "Download worker unavailable");
    httpd_req_async_handler_complete(job->request);
    capture_store_release();
    free(job);
  }
  return ESP_OK;
}

esp_err_t capture_http_register(httpd_handle_t server) {
  const httpd_uri_t routes[] = {
    {.uri = "/api/capture", .method = HTTP_GET, .handler = capture_status_http},
    {.uri = "/api/capture/start", .method = HTTP_POST, .handler = capture_control_http},
    {.uri = "/api/capture/stop", .method = HTTP_POST, .handler = capture_control_http},
    {.uri = "/api/capture/clear", .method = HTTP_POST, .handler = capture_control_http},
    {.uri = "/api/capture/download", .method = HTTP_GET,
     .handler = capture_download_http},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    esp_err_t error = httpd_register_uri_handler(server, &routes[i]);
    if (error != ESP_OK) return error;
  }
  return ESP_OK;
}
