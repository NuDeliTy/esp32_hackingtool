idf_component_register(
    SRCS "hackingtool.c" "ble_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES spi_flash htool_fonts htool_api
)

target_link_libraries(${COMPONENT_LIB} -Wl,-zmuldefs)
