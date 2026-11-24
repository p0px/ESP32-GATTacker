#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t TickType_t;
typedef int *SemaphoreHandle_t;
static int semaphore;
static TickType_t now;
static void (*on_wait)(void), (*on_send)(void);
static unsigned sends, warnings;
static bool enqueue_ok = true;
static atomic_bool hooks_enabled = true;
static atomic_int hook_fault = 0;
static int hook_mux;
static uint32_t next_hook_id, pending_hook_id, stale_id;
static char *hook_reply;
static SemaphoreHandle_t hook_signal;
static int gattAttackState;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &semaphore; }
static TickType_t xTaskGetTickCount(void) { return now; }
static int xSemaphoreGive(SemaphoreHandle_t s) { *s = 1; return 1; }
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  if (*s) { *s = 0; return 1; }
  if (wait && on_wait) {
    void (*callback)(void) = on_wait;
    on_wait = NULL;
    callback();
  }
  if (*s) { *s = 0; return 1; }
  now += wait;
  return 0;
}
typedef struct { int unused; } cJSON;
static cJSON *cJSON_CreateObject(void) { static cJSON j; return &j; }
#define cJSON_AddBoolToObject(json, key, value) ((void)(json), (void)(key), (void)(value))
#define cJSON_AddNumberToObject(json, key, value) ((void)(json), (void)(key), (void)(value))
#define cJSON_AddStringToObject(json, key, value) ((void)(json), (void)(key), (void)(value))
static char *cJSON_PrintUnformatted(cJSON *json) { (void)json; return strdup("{}"); }
static void cJSON_Delete(cJSON *json) { (void)json; }
static bool try_msg_clients(const char *json, bool hook) {
  assert(json && hook);
  ++sends;
  if (on_send) on_send();
  return enqueue_ok;
}
static void gatt_send_output(const char *msg, bool json) { assert(msg && !json); ++warnings; }
static void gatt_send_state(int state) { (void)state; }
typedef struct { const char *action, *hook_ret; uint32_t request_id; } Params;
#include "hook_production.inc"

static void deliver(uint32_t id, const char *value) {
  Params params = {"hookret", value, id};
  receive(&params);
}
static void reset(void) {
  assert(!pending_hook_id && !hook_reply);
  hooks_enabled = true;
  hook_fault = 0;
  hook_timeout_streak = 0;
  hook_queue_failure_streak = 0;
  hook_last_ms = 0;
  semaphore = 0;
  now = sends = warnings = 0;
  enqueue_ok = true;
  on_wait = on_send = NULL;
}
static void duplicate_reply(void) {
  deliver(pending_hook_id, "01");
  deliver(pending_hook_id, "02");
}
static void delayed_reply(void) { now += 120; deliver(pending_hook_id, "03"); }
static void current_reply(void) { now++; deliver(pending_hook_id, "04"); }
static void stale_reply(void) {
  now++;
  deliver(stale_id, "ff");
  xSemaphoreGive(hook_signal);
  on_wait = current_reply;
}
static void cancel(void) { hooks_enabled = false; xSemaphoreGive(hook_signal); }
int main(void) {
  reset();
  on_send = duplicate_reply;
  char *reply = run_hook("read", "uuid", "00");
  assert(reply && !strcmp(reply, "01") && now == 0);
  free(reply);
  reset();
  on_wait = delayed_reply;
  reply = run_hook("write", "uuid", "00");
  assert(reply && !strcmp(reply, "03") && now == 120 && hook_last_ms == 120);
  free(reply);
  reset();
  stale_id = next_hook_id;
  on_wait = stale_reply;
  reply = run_hook("read", "uuid", "00");
  assert(reply && !strcmp(reply, "04") && now == 2);
  free(reply);
  reset();
  assert(!run_hook("read", "uuid", "00"));
  assert(now == 250 && hooks_enabled && !hook_fault && warnings == 1 && !pending_hook_id);
  deliver(next_hook_id, "ff");
  assert(!hook_reply); // A late response cannot alter the next transaction.
  on_wait = delayed_reply;
  reply = run_hook("read", "uuid", "00");
  assert(reply && hooks_enabled && !hook_timeout_streak);
  free(reply);
  assert(!run_hook("read", "uuid", "00") && hooks_enabled);
  assert(!run_hook("read", "uuid", "00") && hooks_enabled);
  assert(!run_hook("read", "uuid", "00") && !hooks_enabled && hook_fault == 1);
  assert(hook_last_ms == 250 && warnings == 4 && sends == 5);
  assert(!run_hook("read", "uuid", "00") && sends == 5);
  reset();
  now = UINT32_MAX - 100;
  assert(!run_hook("read", "uuid", "00") && hooks_enabled && hook_last_ms == 250);
  reset();
  enqueue_ok = false;
  assert(!run_hook("read", "uuid", "00"));
  assert(now == 0 && hooks_enabled && !hook_fault && hook_queue_failure_streak == 1);
  assert(!run_hook("read", "uuid", "00") && hooks_enabled);
  assert(!run_hook("read", "uuid", "00") && !hooks_enabled && hook_fault == 2);
  assert(warnings == 3);
  reset();
  enqueue_ok = false;
  assert(!run_hook("read", "uuid", "00") && hooks_enabled);
  enqueue_ok = true;
  on_send = duplicate_reply;
  reply = run_hook("read", "uuid", "00");
  assert(reply && !hook_queue_failure_streak && hooks_enabled);
  free(reply);
  reset();
  on_wait = cancel;
  assert(!run_hook("read", "uuid", "00") && now == 0 && warnings == 0);
}
