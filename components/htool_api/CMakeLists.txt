idf_component_register(SRCS "htool_api.c"
                       INCLUDE_DIRS "include"
                       REQUIRES htool_wifi htool_display htool_uart htool_ble htool_pn532_spi htool_ir)