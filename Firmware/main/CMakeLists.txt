idf_component_register(SRC_DIRS "." "ble" "wifi"
                       INCLUDE_DIRS "../include")

# for bypassing not being able to send deauth packets
target_link_libraries(${COMPONENT_LIB} -Wl,-zmuldefs)
