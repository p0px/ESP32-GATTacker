"""Compile the actual hook exchange against a deterministic fake RTOS/transport.
Run: python3 Firmware/tests/hook_exchange_test.py (requires g++).
"""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "main/ble/GattAttackApp.cpp").read_text()
run = source[source.index("char* run_hook("):source.index("void send_output(")]
reply = source[source.index('  if (strcmp(ps->action, "hookret")'):source.index('  if (strcmp(ps->action, "enable_hooks")')]
preamble = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
using TickType_t = uint32_t;
using SemaphoreHandle_t = int*;
static int semaphore;
static TickType_t now;
static std::function<void()> on_wait, on_send;
static unsigned sends, warnings;
static bool enqueue_ok = true;
static std::atomic<bool> hooks_enabled{true};
static std::atomic<int> hook_fault{0};
static int hook_mux;
static uint32_t next_hook_id, pending_hook_id;
static char *hook_reply;
static SemaphoreHandle_t hook_signal;
static constexpr uint32_t HOOK_TIMEOUT_MS = 50;
static int gattAttackState;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
SemaphoreHandle_t xSemaphoreCreateBinary() { return &semaphore; }
TickType_t xTaskGetTickCount() { return now; }
int xSemaphoreGive(SemaphoreHandle_t s) { *s = 1; return 1; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  if (*s) { *s = 0; return 1; }
  if (wait && on_wait) { auto f = on_wait; on_wait = {}; f(); }
  if (*s) { *s = 0; return 1; }
  now += wait;
  return 0;
}
struct cJSON {};
cJSON *cJSON_CreateObject() { static cJSON j; return &j; }
void cJSON_AddBoolToObject(cJSON*, const char*, bool) {}
void cJSON_AddNumberToObject(cJSON*, const char*, unsigned) {}
void cJSON_AddStringToObject(cJSON*, const char*, const char*) {}
char *cJSON_PrintUnformatted(cJSON*) { return strdup("{}"); }
void cJSON_Delete(cJSON*) {}
bool try_msg_clients(const char*, bool) {
  ++sends;
  if (on_send) on_send();
  return enqueue_ok;
}
void send_output(const char*) { ++warnings; }
struct GattAttackApp { static void sendState(int) {} };
struct Params { const char *action = "hookret"; const char *hook_ret; uint32_t request_id; };
"""
tests = r"""
void deliver(uint32_t id, const char *value) { Params p; p.request_id=id; p.hook_ret=value; receive(&p); }
void reset() {
  assert(!pending_hook_id && !hook_reply);
  hooks_enabled = true; hook_fault = 0; semaphore = 0; now = 0; sends = warnings = 0;
  enqueue_ok = true; on_wait = {}; on_send = {};
}
int main() {
  reset();
  on_send = [] { deliver(pending_hook_id, "01"); deliver(pending_hook_id, "02"); };
  char *r = run_hook("read", "uuid", "00");
  assert(r && std::string(r)=="01" && now==0); free(r);

  reset();
  on_wait = [] { now += 2; deliver(pending_hook_id, "03"); };
  r = run_hook("write", "uuid", "00");
  assert(r && std::string(r)=="03" && now==2); free(r);

  reset();
  const auto stale = next_hook_id;
  on_wait = [stale] { now += 1; deliver(stale, "ff"); xSemaphoreGive(hook_signal);
    on_wait = [] { now += 1; deliver(pending_hook_id, "04"); }; };
  r = run_hook("read", "uuid", "00");
  assert(r && std::string(r)=="04" && now==2); free(r);

  reset();
  assert(!run_hook("read", "uuid", "00"));
  assert(now==50 && !hooks_enabled && hook_fault==1 && warnings==1 && !pending_hook_id);
  deliver(next_hook_id, "ff"); // A timed-out result cannot survive into another call.
  assert(!hook_reply && !run_hook("read", "uuid", "00") && sends==1 && now==50);

  reset(); enqueue_ok = false;
  assert(!run_hook("read", "uuid", "00") && now==0 && !hooks_enabled && hook_fault==2 && warnings==1);

  reset();
  on_wait = [] { hooks_enabled=false; xSemaphoreGive(hook_signal); };
  assert(!run_hook("read", "uuid", "00") && now==0 && warnings==0);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / "hook_test.cpp"
    binary = Path(tmp) / "hook_test"
    cpp.write_text(preamble + run + "\nvoid receive(Params *ps) {\n" + reply + "\n}\n" + tests)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Hook exchange tests passed")
