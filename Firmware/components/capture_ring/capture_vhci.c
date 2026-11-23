#include "capture_ring.h"
#include "esp_bt.h"

// The existing controller callbacks run unchanged. Only bounded packet copies occur here.
static CaptureRing *capture;
static esp_vhci_host_callback_t original;

void capture_vhci_attach(CaptureRing *ring) {
  capture = ring;
}

void __real_esp_vhci_host_send_packet(uint8_t *data, uint16_t length);
esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *callback);

static void send_available(void) {
  if (original.notify_host_send_available) original.notify_host_send_available();
}

static int packet_received(uint8_t *data, uint16_t length) {
  capture_ring_record(capture, data, length, true);
  return original.notify_host_recv ? original.notify_host_recv(data, length) : 0;
}

void __wrap_esp_vhci_host_send_packet(uint8_t *data, uint16_t length) {
  capture_ring_record(capture, data, length, false);
  __real_esp_vhci_host_send_packet(data, length);
}

esp_err_t __wrap_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *callback) {
  if (!capture || !callback) return __real_esp_vhci_host_register_callback(callback);
  original = *callback;
  static const esp_vhci_host_callback_t observer = {send_available, packet_received};
  return __real_esp_vhci_host_register_callback(&observer);
}
