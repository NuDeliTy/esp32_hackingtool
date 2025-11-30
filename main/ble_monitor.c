/*
 * ble_monitor.c - Full UART (TX + RX)
 * - Adds RX Characteristic (Fixes "Disconnected" status in app)
 * - Enables sending commands from Phone -> ESP32
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_timer.h" 
#include "ble_monitor.h"

#define TAG "BLE_MONITOR"
#define DEVICE_NAME "ESP32_HACKTOOL" 

// Nordic UART Service UUIDs
// Service: 6E400001...
// RX (Write): 6E400002...
// TX (Notify): 6E400003...
static uint8_t service_uuid128[16] = {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E};
#define CHAR_RX_UUID           {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E}
#define CHAR_TX_UUID           {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E}

static uint16_t gatts_if_handle = ESP_GATT_IF_NONE;
static uint16_t tx_char_handle = 0;
static uint16_t rx_char_handle = 0;
static uint16_t conn_id = 0;
static uint16_t service_handle = 0;

static bool device_connected = false;
static bool monitor_active = false;
static int64_t connection_time = 0; 

// Log Hook to capture prints
vprintf_like_t original_log_handler = NULL;

void ble_send_log(const char *data, int len) {
    if (!monitor_active || !device_connected || gatts_if_handle == ESP_GATT_IF_NONE || tx_char_handle == 0) return;
    if ((esp_timer_get_time() - connection_time) < 1000000) return; // Warmup delay

    int sent = 0;
    if (len > 256) len = 256; 

    while (sent < len) {
        int chunk_len = (len - sent) > 20 ? 20 : (len - sent);
        esp_err_t err = esp_ble_gatts_send_indicate(gatts_if_handle, conn_id, tx_char_handle, chunk_len, (uint8_t *)&data[sent], false);
        if (err == ESP_OK) sent += chunk_len;
        else break; 
        esp_rom_delay_us(15000); 
    }
}

int ble_log_hook(const char *fmt, va_list args) {
    if (device_connected) {
        char buf[256];
        int len = vsnprintf(buf, sizeof(buf), fmt, args);
        if (len > 0) ble_send_log(buf, len);
    }
    if (original_log_handler) return original_log_handler(fmt, args);
    return vprintf(fmt, args);
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (!monitor_active) return;
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
            .adv_int_min = 0x20, .adv_int_max = 0x40,
            .adv_type = ADV_TYPE_IND, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .channel_map = ADV_CHNL_ALL, 
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        });
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    if (!monitor_active) return;
    switch (event) {
        case ESP_GATTS_REG_EVT:
            gatts_if_handle = gatts_if;
            esp_ble_gap_set_device_name(DEVICE_NAME);
            esp_ble_gap_config_adv_data(&(esp_ble_adv_data_t){
                .set_scan_rsp = false, .include_name = true, .include_txpower = true,
                .min_interval = 0x0006, .max_interval = 0x0010, .appearance = 0x00, 
                .manufacturer_len = 0, .p_manufacturer_data =  NULL,
                .service_data_len = 0, .p_service_data = NULL, 
                .service_uuid_len = 16, .p_service_uuid = service_uuid128, 
                .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
            });
            esp_gatt_srvc_id_t service_id = { .id.uuid.len = ESP_UUID_LEN_128 };
            memcpy(service_id.id.uuid.uuid.uuid128, service_uuid128, 16);
            esp_ble_gatts_create_service(gatts_if, &service_id, 6); // Handle count increased
            break;

        case ESP_GATTS_CREATE_EVT:
            {
                service_handle = param->create.service_handle;
                
                // 1. Create RX Characteristic (Write)
                uint8_t rx_uuid128[] = CHAR_RX_UUID;
                esp_bt_uuid_t rx_uuid = { .len = ESP_UUID_LEN_128 };
                memcpy(rx_uuid.uuid.uuid128, rx_uuid128, 16);
                esp_ble_gatts_add_char(service_handle, &rx_uuid,
                                       ESP_GATT_PERM_WRITE, 
                                       ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR, 
                                       NULL, NULL);

                // 2. Create TX Characteristic (Notify)
                uint8_t tx_uuid128[] = CHAR_TX_UUID;
                esp_bt_uuid_t tx_uuid = { .len = ESP_UUID_LEN_128 };
                memcpy(tx_uuid.uuid.uuid128, tx_uuid128, 16);
                esp_ble_gatts_add_char(service_handle, &tx_uuid,
                                       ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_NOTIFY, NULL, NULL);
                                       
                esp_ble_gatts_start_service(service_handle);
            }
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            {
                // Check which UUID was added to assign handles correctly
                uint8_t rx_uuid128[] = CHAR_RX_UUID;
                if (memcmp(param->add_char.char_uuid.uuid.uuid128, rx_uuid128, 16) == 0) {
                    rx_char_handle = param->add_char.attr_handle;
                } else {
                    tx_char_handle = param->add_char.attr_handle;
                }
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            conn_id = param->connect.conn_id;
            device_connected = true;
            connection_time = esp_timer_get_time();
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            device_connected = false;
            if (monitor_active) {
                esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
                    .adv_int_min = 0x20, .adv_int_max = 0x40,
                    .adv_type = ADV_TYPE_IND, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                    .channel_map = ADV_CHNL_ALL, 
                    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
                });
            }
            break;

        // --- NEW: Handle Incoming Data from Phone ---
        case ESP_GATTS_WRITE_EVT:
            if (param->write.handle == rx_char_handle && param->write.len > 0) {
                // Ensure null termination for string safety
                char *cmd = malloc(param->write.len + 1);
                memcpy(cmd, param->write.value, param->write.len);
                cmd[param->write.len] = 0;

                // Log it to USB Serial so you see it in VS Code
                if (original_log_handler) { // Use original handler to avoid loop
                    printf("BLE CMD RECEIVED: %s\n", cmd);
                }

                // Simple Test Logic
                if (strstr(cmd, "ping")) {
                    printf("pong\n"); // This will go back to phone via the hook
                }

                free(cmd);
                
                // Send response if needed (optional)
                if (param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
                }
            }
            break;

        default: break;
    }
}

void stop_ble_monitor() {
    if (!monitor_active) return;
    monitor_active = false;
    device_connected = false;
    esp_ble_gap_stop_advertising();
    if (original_log_handler) {
        esp_log_set_vprintf(original_log_handler);
        original_log_handler = NULL;
    }
    printf("BLE Monitor Paused\n");
}

void init_ble_monitor() {
    monitor_active = true;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
         esp_bt_controller_init(&bt_cfg);
         esp_bt_controller_enable(ESP_BT_MODE_BLE);
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_init();
        esp_bluedroid_enable();
    }
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_app_register(0);

    if (!original_log_handler) {
        original_log_handler = esp_log_set_vprintf(ble_log_hook);
    }
    printf("BLE Monitor Started. Connect to 'ESP32_HACKTOOL'.\n");
}