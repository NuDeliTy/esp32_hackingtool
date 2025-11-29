/*
 * ble_monitor.c - Throttled & Stable for iOS (Fixed Build)
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
#include "ble_monitor.h"

#define TAG "BLE_MONITOR"
#define DEVICE_NAME "ESP32_HACKTOOL" 

// Nordic UART Service UUIDs
#define SERVICE_UUID           {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E}
#define CHAR_TX_UUID           {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E}

static uint16_t gatts_if_handle = ESP_GATT_IF_NONE;
static uint16_t tx_char_handle = 0;
static bool device_connected = false;
static bool monitor_active = false;
static uint16_t conn_id = 0; 
static uint16_t service_handle = 0; // <--- ADDED MISSING VARIABLE

// Log Hook to capture prints
vprintf_like_t original_log_handler = NULL;

void ble_send_log(const char *data, int len) {
    if (monitor_active && device_connected && gatts_if_handle != ESP_GATT_IF_NONE && tx_char_handle != 0) {
        int sent = 0;
        
        // LIMIT: Don't try to send massive logs at once, limit to 256 bytes per call
        if (len > 256) len = 256; 

        while (sent < len) {
            // Nordic UART usually supports 20 bytes per packet by default
            int chunk_len = (len - sent) > 20 ? 20 : (len - sent);
            
            // Try to send. If fail (buffer full), just abort this chunk to save connection.
            esp_err_t err = esp_ble_gatts_send_indicate(gatts_if_handle, conn_id, tx_char_handle, chunk_len, (uint8_t *)&data[sent], false);
            
            if (err == ESP_OK) {
                sent += chunk_len;
            } else {
                // Buffer full or congestion. Stop sending this log line.
                // Do NOT retry loop, or we will disconnect.
                break; 
            }
            
            // Critical delay for iOS stability (prevents flooding)
            esp_rom_delay_us(15000); // 15ms delay
        }
    }
}

int ble_log_hook(const char *fmt, va_list args) {
    // Only process if someone is listening
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
                .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
            });
            esp_gatt_srvc_id_t service_id = { .id.uuid.len = ESP_UUID_LEN_128 };
            uint8_t service_uuid128[] = SERVICE_UUID;
            memcpy(service_id.id.uuid.uuid.uuid128, service_uuid128, 16);
            esp_ble_gatts_create_service(gatts_if, &service_id, 4);
            break;

        case ESP_GATTS_CREATE_EVT:
            {
                service_handle = param->create.service_handle;
                uint8_t char_uuid128[] = CHAR_TX_UUID;
                esp_bt_uuid_t char_uuid = { .len = ESP_UUID_LEN_128 };
                memcpy(char_uuid.uuid.uuid128, char_uuid128, 16);
                esp_ble_gatts_add_char(param->create.service_handle, &char_uuid,
                                       ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_NOTIFY, NULL, NULL);
                esp_ble_gatts_start_service(param->create.service_handle);
            }
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            tx_char_handle = param->add_char.attr_handle;
            break;
        case ESP_GATTS_CONNECT_EVT:
            conn_id = param->connect.conn_id; // Store connection ID
            device_connected = true;
            
            // Request iOS-friendly connection parameters
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.latency = 0;
            conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 400;     // timeout = 400*10ms = 4000ms
            esp_ble_gap_update_conn_params(&conn_params);
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
    // Check if BT controller is already init (shared with other BLE tools)
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