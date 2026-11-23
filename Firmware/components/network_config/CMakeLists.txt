idf_component_register(
  SRCS "network_config.c" "network_service.c"
  INCLUDE_DIRS "." "../../include"
  REQUIRES json bt esp_wifi esp_netif esp_event esp_timer nvs_flash
)
