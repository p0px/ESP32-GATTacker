#include "network_pair.h"
#include "control_http.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <string.h>

enum { GRANTS = 4 };
static const int64_t APPROVAL_US = 60000000;
static const int64_t GRANT_US = 8LL * 60 * 60 * 1000000;

typedef enum { PairNone, PairWaiting, PairAllowed, PairRejected } PairResult;
typedef struct {
  char ticket[33];
  char peer[16];
  int64_t expires;
} Grant;

typedef struct {
  SemaphoreHandle_t mutex;
  NetworkPairConnected connected;
  NetworkPairRequest send;
  void *context;
  char ticket[33];
  char peer[16];
  uint32_t id;
  int64_t deadline;
  PairResult result;
  Grant grants[GRANTS];
  unsigned next_grant;
} NetworkPair;

static NetworkPair pair;

void network_pair_bind(NetworkPairConnected connected, NetworkPairRequest send,
                       void *context) {
  if (!pair.mutex) pair.mutex = xSemaphoreCreateMutex();
  if (!pair.mutex) return;
  xSemaphoreTake(pair.mutex, portMAX_DELAY);
  pair.connected = connected;
  pair.send = send;
  pair.context = context;
  xSemaphoreGive(pair.mutex);
}

static bool peer_ip(httpd_req_t *request, char out[16]) {
  struct sockaddr_storage peer;
  socklen_t size = sizeof(peer);
  if (getpeername(httpd_req_to_sockfd(request), (struct sockaddr *)&peer, &size))
    return false;
  if (peer.ss_family == AF_INET)
    return inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, out, 16) != NULL;
  if (peer.ss_family != AF_INET6) return false;
  const struct in6_addr *ip = &((struct sockaddr_in6 *)&peer)->sin6_addr;
  return IN6_IS_ADDR_V4MAPPED(ip) &&
         inet_ntop(AF_INET, &ip->s6_addr[12], out, 16) != NULL;
}

static void random_ticket(char out[33]) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  for (unsigned i = 0; i < sizeof(bytes); i++)
    snprintf(out + i * 2, 3, "%02x", bytes[i]);
  memset(bytes, 0, sizeof(bytes));
}

static bool same_ticket(const char *left, const char *right) {
  if (!right || strlen(right) != 32) return false;
  unsigned difference = 0;
  for (unsigned i = 0; i < 32; i++) difference |= left[i] ^ right[i];
  return difference == 0;
}

static esp_err_t result_json(httpd_req_t *request, const char *state, const char *ticket) {
  cJSON *json = cJSON_CreateObject();
  if (!json) return control_error(request, "503 Service Unavailable", "Out of memory");
  cJSON_AddStringToObject(json, "state", state);
  if (ticket) cJSON_AddStringToObject(json, "ticket", ticket);
  return control_json(request, json);
}

esp_err_t network_pair_start_http(httpd_req_t *request) {
  if (!control_origin(request))
    return control_error(request, "403 Forbidden", "Open this page at the board's IP address");
  if (request->content_len)
    return control_error(request, "400 Bad Request", "Pair request must be empty");
  if (!pair.mutex || !pair.connected || !pair.connected(pair.context))
    return control_error(request, "409 Conflict", "Open GATTacker on the Flipper first");
  char peer[16], ticket[33];
  if (!peer_ip(request, peer))
    return control_error(request, "403 Forbidden", "An IPv4 browser address is required");
  xSemaphoreTake(pair.mutex, portMAX_DELAY);
  int64_t now = esp_timer_get_time();
  bool busy = pair.result == PairWaiting && now < pair.deadline;
  uint32_t id = 0;
  NetworkPairRequest send = pair.send;
  void *context = pair.context;
  if (!busy) {
    random_ticket(pair.ticket);
    snprintf(pair.peer, sizeof(pair.peer), "%s", peer);
    pair.id = esp_random() | 1;
    pair.deadline = now + APPROVAL_US;
    pair.result = PairWaiting;
    snprintf(ticket, sizeof(ticket), "%s", pair.ticket);
    id = pair.id;
  }
  xSemaphoreGive(pair.mutex);
  if (busy) return control_error(request, "409 Conflict", "Approval already pending");
  if (send) send(context, id, peer);
  return result_json(request, "pending", ticket);
}

esp_err_t network_pair_status_http(httpd_req_t *request) {
  if (!control_origin(request))
    return control_error(request, "403 Forbidden", "Open this page at the board's IP address");
  cJSON *body = control_read(request);
  const cJSON *item = body ? cJSON_GetObjectItemCaseSensitive(body, "ticket") : NULL;
  char ticket[33] = {0};
  if (cJSON_IsString(item) && strlen(item->valuestring) == 32)
    snprintf(ticket, sizeof(ticket), "%s", item->valuestring);
  cJSON_Delete(body);
  if (!ticket[0]) return control_error(request, "400 Bad Request", "Pair ticket required");
  char peer[16];
  if (!pair.mutex || !peer_ip(request, peer))
    return control_error(request, "404 Not Found", "Pair request unavailable");
  xSemaphoreTake(pair.mutex, portMAX_DELAY);
  bool matched = same_ticket(pair.ticket, ticket) && !strcmp(pair.peer, peer);
  const char *state = "expired";
  if (matched && pair.result == PairAllowed) {
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < GRANTS; i++) {
      const Grant *grant = &pair.grants[i];
      if (grant->expires > now && !strcmp(grant->peer, peer) &&
          same_ticket(grant->ticket, ticket)) state = "approved";
    }
  } else if (matched && esp_timer_get_time() < pair.deadline) {
    state = pair.result == PairRejected ? "denied" : "pending";
  }
  xSemaphoreGive(pair.mutex);
  if (!matched) return control_error(request, "404 Not Found", "Pair request unavailable");
  return result_json(request, state, NULL);
}

bool network_pair_authorized(httpd_req_t *request, const char *ticket) {
  char peer[16];
  if (!pair.mutex || !peer_ip(request, peer)) return false;
  xSemaphoreTake(pair.mutex, portMAX_DELAY);
  int64_t now = esp_timer_get_time();
  bool authorized = false;
  for (unsigned i = 0; i < GRANTS; i++) {
    const Grant *grant = &pair.grants[i];
    authorized |= grant->expires > now && !strcmp(grant->peer, peer) &&
                  same_ticket(grant->ticket, ticket);
  }
  xSemaphoreGive(pair.mutex);
  return authorized;
}

bool network_pair_decide(uint32_t id, bool allow) {
  if (!pair.mutex) return false;
  xSemaphoreTake(pair.mutex, portMAX_DELAY);
  bool pending = pair.result == PairWaiting && pair.id == id &&
                 esp_timer_get_time() < pair.deadline;
  if (pending) {
    if (allow) {
      Grant *grant = &pair.grants[pair.next_grant++ % GRANTS];
      snprintf(grant->ticket, sizeof(grant->ticket), "%s", pair.ticket);
      snprintf(grant->peer, sizeof(grant->peer), "%s", pair.peer);
      grant->expires = esp_timer_get_time() + GRANT_US;
    }
    pair.result = allow ? PairAllowed : PairRejected;
  }
  xSemaphoreGive(pair.mutex);
  return pending;
}
