idf_component_register(SRC_DIRS "." "ble" "wifi"
                       INCLUDE_DIRS "../include" "../components/capture_ring"
                                    "../components/network_config")

# for bypassing not being able to send deauth packets
target_link_libraries(${COMPONENT_LIB}
  -Wl,-zmuldefs
  -Wl,--wrap=esp_vhci_host_send_packet
  -Wl,--wrap=esp_vhci_host_register_callback)
