idf_component_register(
  SRCS "capture_ring.c" "capture_vhci.c"
  INCLUDE_DIRS "."
  REQUIRES bt esp_timer pcapng
)
