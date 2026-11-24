#include "capture/capture_wire.h"
#include "capture/capture_export.h"
#include "capture/capture_store.h"
#include "capture/flipper_sd.h"
#include "capture_codec.h"
#include "uart/uart_bridge_internal.h"
#include "wire.h"
#include <stdio.h>
#include <string.h>

static void reply(uint16_t id, const char *text) {
  uart_emit(WireReply, id, text);
}

static void summary(const CaptureStats *stats, char *out, size_t size, const char *name) {
  if (!strcmp(name, "capture_health"))
    snprintf(out, size, "%s\t%lu\t%lu\t%lu\t%lu\t%u\t%u", name,
             (unsigned long)stats->token, (unsigned long)stats->accepted,
             (unsigned long)stats->full, (unsigned long)stats->invalid,
             stats->peak, CAPTURE_RING_CAPACITY);
  else
    snprintf(out, size, "%s\t%lu\t%lu\t%lu", name,
             (unsigned long)stats->token, (unsigned long)stats->accepted,
             (unsigned long)(stats->full + stats->invalid));
}

static void ram_status(void) {
  CaptureStoreStatus state = capture_store_status();
  CaptureExportStatus export = capture_export_status();
  const char *names[] = {"empty", "recording", "saving", "ready"};
  char event[200];
  snprintf(event, sizeof(event),
           "capture_ram\t%lu\t%s\t%lu\t%lu\t%u\t%u\t%u\t%s\t%u\t%u",
           (unsigned long)state.id, names[state.state], (unsigned long)state.packets,
           (unsigned long)state.dropped, (unsigned)state.bytes,
           (unsigned)state.capacity, state.auto_export,
           export.id == state.id ? export.state : "idle", (unsigned)export.bytes,
           state.export_failed);
  uart_emit(WireEvent, 0, event);
}

void capture_wire_status(void) {
  ram_status();
  CaptureRing *ring = capture_store_ring();
  if (!ring) return;
  CaptureStats stats;
  bool finished;
  portENTER_CRITICAL(&ring->lock);
  bool web = ring->web;
  stats = ring->stats;
  finished = ring->finished;
  portEXIT_CRITICAL(&ring->lock);
  if (web) return;
  char event[120];
  if (stats.token) {
    summary(&stats, event, sizeof(event), "capture_health");
    uart_emit(WireEvent, 0, event);
    summary(&stats, event, sizeof(event), finished ? "capture_end" : "capture_stats");
  } else snprintf(event, sizeof(event), "capture_idle");
  uart_emit(WireEvent, 0, event);
}

bool capture_wire_receive(char **fields, size_t count) {
  if (flipper_sd_receive(fields, count)) return true;
  return capture_export_receive(fields, count);
}

static const char *direct_command(char **fields, size_t count) {
  CaptureRing *ring = capture_store_ring();
  if (!ring) return "error\tHCI capture requires PSRAM";
  uint32_t token, seconds = 0;
  if (count < 2 || !wire_number(fields[1], UINT32_MAX, &token) || !token)
    return "error\tinvalid capture token";
  bool keepalive = !strcmp(fields[0], "capture_keepalive");
  bool stop = !strcmp(fields[0], "capture_stop");
  if (!keepalive && !stop &&
      (count != 3 || !wire_number(fields[2], UINT32_MAX, &seconds)))
    return "error\tinvalid capture time";
  if (keepalive || stop) {
    if (count != 2) return "error\tinvalid capture command";
    if (keepalive) return capture_ring_keepalive(ring, token) ?
                          "ok\tcapture heartbeat" : "error\tcapture token mismatch";
    return capture_ring_stop(ring, token, false) ?
           "ok\tsaving capture" : "error\tcapture token mismatch";
  }
  const char *error = capture_ring_begin(ring, token, seconds, false);
  return error ? "error\tcapture busy; stop current recorder first" :
                 "ok\trecording HCI packets";
}

static const char *ram_start(char *seconds_text, char response[64]) {
  uint32_t seconds;
  if (!wire_number(seconds_text, UINT32_MAX, &seconds) || !seconds)
    return "error\tinvalid capture timestamp";
  CaptureStoreStatus previous = capture_store_status();
  if (previous.state == CaptureReady && previous.auto_export) {
    FlipperSdStatus sd = flipper_sd_status();
    CaptureExportStatus export = capture_export_status();
    if (sd.complete && sd.id == previous.id && export.id == previous.id &&
        !strcmp(export.state, "complete")) capture_store_clear(previous.id);
  }
  const char *error = capture_store_start(seconds, true, true);
  if (error) {
    snprintf(response, 64, "error\t%s", error);
    return response;
  }
  CaptureStoreStatus state = capture_store_status();
  snprintf(response, 64, "ok\t%lu", (unsigned long)state.id);
  return response;
}

bool capture_wire_command(uint16_t id, char **fields, size_t count) {
  const char *name = fields[0];
  bool direct = !strcmp(name, "capture_start") || !strcmp(name, "capture_stop") ||
                !strcmp(name, "capture_keepalive");
  if (!direct && strcmp(name, "capture_ram_start") && strcmp(name, "capture_ram_stop"))
    return false;
  const char *result = NULL;
  char response[160];
  if (direct) result = direct_command(fields, count);
  else if (!strcmp(name, "capture_ram_start"))
    result = count == 2 ? ram_start(fields[1], response) :
            "error\tinvalid capture timestamp";
  else {
    uint32_t token;
    if (count != 2 || !wire_number(fields[1], UINT32_MAX, &token) || !token)
      result = "error\tinvalid capture id";
    else {
      const char *error = capture_store_stop(token);
      if (error) {
        snprintf(response, sizeof(response), "error\t%s", error);
        result = response;
      } else result = "ok\tstopping";
    }
  }
  reply(id, result);
  capture_wire_status();
  return true;
}

bool capture_wire_poll(char *out, size_t size) {
  CaptureRing *ring = capture_store_ring();
  if (!ring) return false;
  CapturePacket packet;
  CaptureStats stats;
  CaptureEvent event = capture_ring_next(ring, false, &packet, &stats);
  if (event == CaptureNone) return false;
  if (event == CaptureBegin) {
    snprintf(out, size, "capture_begin\t%lu", (unsigned long)stats.token);
    return true;
  }
  if (event == CaptureHealth || event == CaptureEnd) {
    summary(&stats, out, size, event == CaptureEnd ? "capture_end" : "capture_health");
    return true;
  }
  int prefix = snprintf(out, size, "hci\t%lu\t%lu\t%llu\t%u\t",
                        (unsigned long)stats.token, (unsigned long)packet.sequence,
                        (unsigned long long)packet.timestamp, packet.received ? 1U : 0U);
  return prefix > 0 && (size_t)prefix < size &&
         capture_base64(packet.bytes, packet.length, out + prefix, size - prefix) != 0;
}
