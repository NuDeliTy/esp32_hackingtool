/*
 * hackingtool.c - FIXED: TV-B-Gone Submenu & Category Logic
 */

#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "esp_event.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "htool_api.h"
#include "htool_netman.h"
#include "htool_ble.h"
#include "htool_wifi.h" 
#include "htool_uart.h" 
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "font6x8.h"
#include "ble_monitor.h"

#define TAG "htool_ui"

// --- CONFIGURATION ---
#define HIDE_DUPLICATES 1 

// --- PINS ---
#define PIN_SDA     21
#define PIN_SCL     22

// Button Mapping
#define PIN_BTN_UP    32  
#define PIN_BTN_DOWN  33  
#define PIN_BTN_OK    25  
#define PIN_BTN_BACK  34  

// --- OLED ---
#define SH1106_ADDR 0x3C
#define SH1106_WIDTH 128
#define SH1106_HEIGHT 64
static uint8_t sh1106_buffer[SH1106_WIDTH * SH1106_HEIGHT / 8];

// --- EXTERNAL VARS ---
extern wifi_ap_record_t *global_scans;
extern uint8_t global_scans_count;
extern volatile bool scan_started;
extern uint8_t menu_cnt; 

// --- DATA LISTS ---
static const char *cp_templates[] = { "Google", "McDonald's", "Facebook", "Apple" };
static const int CP_COUNT = 4;

static const char *et_templates[] = {
    "General", "Huawei", "ASUS", "Tp-Link", "Netgear", "o2", "FritzBox", "Vodafone",
    "Magenta", "1&1", "A1", "Globe", "PLDT", "AT&T", "Swisscom", "Verizon"
};
static const int ET_COUNT = 16;

static const char *et_config_options[] = {
    "Clone SSID & MAC",
    "Clone SSID Only"
};

static const char *beacon_modes[] = { 
    "Random SSIDs", "Router (rand MAC)", "Router (same MAC)", "Funny SSIDs" 
};
static const int BEACON_COUNT = 4;

static const char *ble_modes[] = { 
    "Apple", "Google", "Samsung", "Microsoft", "Random" 
};
static const int BLE_COUNT = 5;

static const char *attack_modes[] = {
    "Deauth Once", "Disassoc Once", "Single Frame", "Continuous"
};
static const int ATTACK_COUNT = 4;

// NEW: IR Categories
static const char *ir_categories[] = { 
    "TVs Only", "Projectors", "Audio Systems", "All Devices" 
};
static const int IR_CAT_COUNT = 4;

// --- STATE MACHINE ---
typedef enum {
    MAIN_MENU = 0,
    SCAN_RESULTS_MENU,
    ATTACK_MENU,
    BEACON_MENU,
    CAPTIVE_MENU,
    EVIL_TWIN_CONFIG_MENU,
    EVIL_TWIN_MENU,
    BLE_MENU,
    TV_B_GONE_MENU
} ui_mode_t;

static const char *main_items[] = { 
    "Scan Networks", "Deauth (toggle)", "Beacon Spammer", "Captive Portal", "Evil Twin", "BLE Spoof", "TV-B-Gone" 
};
static const int MAIN_ITEMS = 7;

static int menu_index = 0;
static ui_mode_t ui_mode = MAIN_MENU;
static int target_ap_index = -1; 
static bool et_clone_mac = true; 

// --- DATA STRUCT FOR UI LIST ---
typedef struct {
    wifi_ap_record_t record;
    int mesh_count; 
} ui_scan_result_t;

static ui_scan_result_t scan_results_local[64]; 
static int scan_count_local = 0;


// --- DISPLAY DRIVER ---
static void sh1106_cmd(uint8_t cmd) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (SH1106_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x00, true);
    i2c_master_write_byte(h, cmd, true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
}

static void sh1106_data(const uint8_t *data, size_t len) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (SH1106_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x40, true);
    i2c_master_write(h, (uint8_t *)data, len, true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
}

static void sh1106_init(void) {
    sh1106_cmd(0xAE); sh1106_cmd(0xD5); sh1106_cmd(0x80);
    sh1106_cmd(0xA8); sh1106_cmd(0x3F); sh1106_cmd(0xD3); sh1106_cmd(0x00);
    sh1106_cmd(0x40); sh1106_cmd(0xAD); sh1106_cmd(0x8B); sh1106_cmd(0xA1);
    sh1106_cmd(0xC8); sh1106_cmd(0xDA); sh1106_cmd(0x12); sh1106_cmd(0x81); 
    sh1106_cmd(0x7F); sh1106_cmd(0xD9); sh1106_cmd(0x22); sh1106_cmd(0xDB); 
    sh1106_cmd(0x35); sh1106_cmd(0xA4); sh1106_cmd(0xA6); sh1106_cmd(0xAF);
}

static void draw_pixel(int x, int y, int color) {
    if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
    size_t idx = x + (y / 8) * SH1106_WIDTH;
    if (color) sh1106_buffer[idx] |= (1 << (y & 7));
    else sh1106_buffer[idx] &= ~(1 << (y & 7));
}

static void draw_char(int x, int y, char ch) {
    const uint8_t *glyph = font6x8[(unsigned char)ch];
    for (int i = 0; i < 6; ++i) {
        uint8_t col = glyph[i];
        for (int j = 0; j < 8; ++j) draw_pixel(x + i, y + j, (col >> j) & 1);
    }
}

static void draw_str(int x, int y, const char *s) {
    while (*s) { draw_char(x, y, *s++); x += 6; }
}

static void clear(void) { memset(sh1106_buffer, 0, sizeof(sh1106_buffer)); }

static void sh1106_update(void) {
    for (uint8_t page = 0; page < 8; page++) {
        sh1106_cmd(0xB0 + page); sh1106_cmd(0x02); sh1106_cmd(0x10);
        sh1106_data(&sh1106_buffer[page * SH1106_WIDTH], SH1106_WIDTH);
    }
}

static void i2c_init(void) {
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER, .sda_io_num = PIN_SDA, .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = PIN_SCL, .scl_pullup_en = GPIO_PULLUP_ENABLE, .master = {.clk_speed = 400000}
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0));
}

// -------------------------------------------------------------
// BUTTONS
// -------------------------------------------------------------
int read_navigation() {
    static int last_up_state = 1;
    static int last_down_state = 1;
    int up = gpio_get_level(PIN_BTN_UP);
    int down = gpio_get_level(PIN_BTN_DOWN);
    int result = 0;
    if (last_up_state == 1 && up == 0) result = -1;
    if (last_down_state == 1 && down == 0) result = 1;
    last_up_state = up;
    last_down_state = down;
    return result;
}

static int read_button(int pin) { 
    return (gpio_get_level(pin) == 0); 
}

// -------------------------------------------------------------
// NVS
// -------------------------------------------------------------
static nvs_handle_t nvs_h = 0;
static int ble_preset = 0;
static int beacon_index = 0;

static void nvs_init_settings(void) {
    if (nvs_open("htool", NVS_READWRITE, &nvs_h) == ESP_OK) {
        int32_t tmp;
        if (nvs_get_i32(nvs_h, "ble_preset", &tmp) == ESP_OK) ble_preset = tmp;
        if (nvs_get_i32(nvs_h, "beacon_idx", &tmp) == ESP_OK) beacon_index = tmp;
    }
}
static void nvs_save_settings(void) {
    if (nvs_h) { nvs_set_i32(nvs_h, "ble_preset", ble_preset); nvs_set_i32(nvs_h, "beacon_idx", beacon_index); nvs_commit(nvs_h); }
}

// --- UI HELPERS ---
static void show_action_msg(const char *line1, const char *line2) {
    clear(); draw_str(0, 0, line1); if (line2) draw_str(0, 12, line2); sh1106_update();
}

// --- SCANNING ---
typedef enum { WIFI_SCAN_IDLE = 0, WIFI_SCAN_RUNNING, WIFI_SCAN_DONE } wifi_scan_state_t;
static volatile wifi_scan_state_t scan_state = WIFI_SCAN_IDLE;

static void wifi_scan_task(void *arg) {
    scan_state = WIFI_SCAN_RUNNING;
    scan_count_local = 0;
    htool_api_start_active_scan();
    vTaskDelay(pdMS_TO_TICKS(200));
    while (scan_started) { vTaskDelay(pdMS_TO_TICKS(100)); }
    
    if (global_scans_count > 0 && global_scans != NULL) {
        int unique_count = 0;
        char current_ssid[33]; 

        for (int i = 0; i < global_scans_count && unique_count < 64; ++i) {
            memset(current_ssid, 0, 33);
            if (strlen((char*)global_scans[i].ssid) == 0) {
                snprintf(current_ssid, 32, "(Hidden) %02X", global_scans[i].bssid[5]);
            } else {
                strncpy(current_ssid, (char*)global_scans[i].ssid, 32);
            }

            #if HIDE_DUPLICATES
                bool is_duplicate = false;
                for (int k = 0; k < unique_count; k++) {
                    if (strcmp(current_ssid, (char*)scan_results_local[k].record.ssid) == 0) {
                        scan_results_local[k].mesh_count++;
                        is_duplicate = true;
                        break;
                    }
                }
                if (!is_duplicate) {
                    scan_results_local[unique_count].record = global_scans[i];
                    strcpy((char*)scan_results_local[unique_count].record.ssid, current_ssid);
                    scan_results_local[unique_count].mesh_count = 1; 
                    unique_count++;
                }
            #else
                scan_results_local[unique_count].record = global_scans[i];
                strcpy((char*)scan_results_local[unique_count].record.ssid, current_ssid);
                scan_results_local[unique_count].mesh_count = 1;
                unique_count++;
            #endif
        }
        scan_count_local = unique_count;
    }
    scan_state = WIFI_SCAN_DONE;
    vTaskDelete(NULL);
}

static void start_nonblocking_scan(void) {
    if (scan_state == WIFI_SCAN_RUNNING) return; 
    scan_state = WIFI_SCAN_RUNNING;
    xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, NULL);
}

static void show_scan_results_screen(int selected) {
    clear(); draw_str(0, 0, "Scan results:");
    int count = (scan_state == WIFI_SCAN_DONE ? scan_count_local : global_scans_count);
    
    if (count <= 0) { draw_str(0, 12, "No results"); sh1106_update(); return; }

    int start = (selected > 3 && count > 6) ? selected - 3 : 0;
    for (int i = 0; i < 6 && (start + i) < count; i++) {
        int idx = start + i;
        char line[40];
        
        int nodes = scan_results_local[idx].mesh_count;
        char mesh_str[16] = ""; 
        if (nodes > 1) snprintf(mesh_str, 15, "(%d)", nodes);

        const char* freq = (scan_results_local[idx].record.primary > 14) ? "5G" : "2.4";
        snprintf(line, 40, "%-10.10s %s %s %d", scan_results_local[idx].record.ssid, freq, mesh_str, scan_results_local[idx].record.rssi);
        
        if (idx == selected) draw_str(0, 12+i*8, "> ");
        draw_str(12, 12+i*8, line);
    }
    sh1106_update();
}

// -------------------------------------------------------------
// MAIN UI LOGIC
// -------------------------------------------------------------
static void ui_task(void *arg) {
    nvs_init_settings();
    clear(); draw_str(0, 0, "HackingTool ready"); sh1106_update();
    vTaskDelay(pdMS_TO_TICKS(500));

    int scan_selected = 0;
    typedef enum { PENDING_NONE, PENDING_EVIL_TWIN } pending_t;
    pending_t pending_op = PENDING_NONE;

    while (1) {
        int nav = read_navigation();
        
        // --- INPUT HANDLING ---
        if (nav) {
            if (ui_mode == MAIN_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0; 
                else if (menu_index >= MAIN_ITEMS) menu_index = MAIN_ITEMS - 1;
            } 
            else if (ui_mode == SCAN_RESULTS_MENU) {
                scan_selected += nav;
                int max = (scan_state == WIFI_SCAN_DONE ? scan_count_local : global_scans_count);
                if (scan_selected < 0) scan_selected = 0; 
                else if (scan_selected >= max) scan_selected = max - 1;
            } 
            else if (ui_mode == BEACON_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0; 
                else if (menu_index >= BEACON_COUNT) menu_index = BEACON_COUNT - 1;
            }
            else if (ui_mode == CAPTIVE_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0; 
                else if (menu_index >= CP_COUNT) menu_index = CP_COUNT - 1;
            }
            else if (ui_mode == EVIL_TWIN_CONFIG_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0; 
                else if (menu_index >= 2) menu_index = 1;
            }
            else if (ui_mode == EVIL_TWIN_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0; 
                else if (menu_index >= ET_COUNT) menu_index = ET_COUNT - 1;
            }
            else if (ui_mode == BLE_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0; 
                else if (menu_index >= BLE_COUNT) menu_index = BLE_COUNT - 1;
            }
            else if (ui_mode == ATTACK_MENU) {
                menu_index += nav;
                if (menu_index < 0) menu_index = 0;
                else if (menu_index >= ATTACK_COUNT) menu_index = ATTACK_COUNT - 1;
            }
            else if (ui_mode == TV_B_GONE_MENU) {
                // Nav for Categories (0-3)
                menu_index += nav;
                if (menu_index < 0) menu_index = 0;
                else if (menu_index >= IR_CAT_COUNT) menu_index = IR_CAT_COUNT - 1;
            }
        }

        // --- DRAWING ---
        if (ui_mode == MAIN_MENU) {
            clear(); draw_str(0, 0, "HackingTool");
            
            // --- SCROLLING LOGIC FIXED ---
            const int MAX_LINES = 6;
            int start_idx = 0;
            if (menu_index >= MAX_LINES) {
                start_idx = menu_index - (MAX_LINES - 1);
            }
            
            for (int i = 0; i < MAX_LINES; ++i) {
                int item_idx = start_idx + i;
                if (item_idx >= MAIN_ITEMS) break;
                
                int y = 12 + i * 8;
                if (item_idx == menu_index) draw_str(0, y, "> ");
                draw_str(12, y, main_items[item_idx]);
                
                // Status Indicators
                if (item_idx == 1 && htool_api_is_deauther_running()) draw_str(90, y, "RUN");
                if (item_idx == 2 && htool_api_is_beacon_spammer_running()) draw_str(90, y, "RUN");
                if (item_idx == 3 && htool_api_is_captive_portal_running()) draw_str(90, y, "RUN");
                if (item_idx == 4 && htool_api_is_evil_twin_running()) draw_str(90, y, "RUN");
                if (item_idx == 6 && htool_api_is_ir_attack_running()) draw_str(90, y, "RUN");
            }
            sh1106_update();
        } 
        else if (ui_mode == SCAN_RESULTS_MENU) {
            if (scan_state == WIFI_SCAN_RUNNING) {
                clear(); draw_str(0, 0, "Scanning WiFi..."); 
                if (scan_count_local > 0) draw_str(0, 20, "Updating..."); 
                sh1106_update();
            } else {
                show_scan_results_screen(scan_selected);
            }
        } 
        else if (ui_mode == ATTACK_MENU) {
            clear(); draw_str(0, 0, "Attack Menu:");
            char ssid_local[64] = {0}; 
            if (global_scans && target_ap_index < global_scans_count)
                snprintf(ssid_local, 63, "Target: %s", scan_results_local[target_ap_index].record.ssid);
            else snprintf(ssid_local, 63, "Target: Unknown");
            draw_str(0, 10, ssid_local);

            for (int i=0; i<ATTACK_COUNT; i++) {
                if(i==menu_index) draw_str(0, 24+i*8, "> ");
                draw_str(12, 24+i*8, attack_modes[i]);
                if (i==3 && htool_api_is_deauther_running()) draw_str(100, 24+i*8, "RUN");
            }
            sh1106_update();
        }
        else if (ui_mode == BEACON_MENU) {
            clear(); draw_str(0, 0, "Beacon Spammer");
            for (int i=0; i<BEACON_COUNT; i++) {
                if(i==menu_index) draw_str(0, 12+i*10, "> ");
                draw_str(12, 12+i*10, beacon_modes[i]);
                if (htool_api_is_beacon_spammer_running() && beacon_task_args.beacon_index == i) {
                    draw_str(90, 12+i*10, "RUN");
                }
            }
            sh1106_update();
        }
        else if (ui_mode == CAPTIVE_MENU) {
            clear(); draw_str(0, 0, "Captive Portal");
            for (int i=0; i<CP_COUNT; i++) {
                if(i==menu_index) draw_str(0, 12+i*10, "> ");
                draw_str(12, 12+i*10, cp_templates[i]);
                if (htool_api_is_captive_portal_running() && captive_portal_task_args.cp_index == i && !captive_portal_task_args.is_evil_twin) {
                    draw_str(90, 12+i*10, "RUN");
                }
            }
            sh1106_update();
        }
        else if (ui_mode == EVIL_TWIN_CONFIG_MENU) {
            clear(); draw_str(0, 0, "Evil Twin Mode:");
            for (int i=0; i<2; i++) {
                if(i==menu_index) draw_str(0, 16+i*10, "> ");
                draw_str(12, 16+i*10, et_config_options[i]);
            }
            sh1106_update();
        }
        else if (ui_mode == EVIL_TWIN_MENU) {
            clear(); draw_str(0, 0, "Evil Twin");
            char ssid_local[64] = {0}; 
            if (global_scans && target_ap_index < global_scans_count)
                snprintf(ssid_local, 63, "Target: %s", scan_results_local[target_ap_index].record.ssid);
            draw_str(0, 10, ssid_local[0] ? ssid_local : "Choose Template:");

            int start_idx = (menu_index > 3) ? menu_index - 3 : 0;
            if (start_idx + 4 > ET_COUNT) start_idx = ET_COUNT - 4;
            
            for (int i=0; i<4; i++) {
                int real_idx = start_idx + i;
                if(real_idx==menu_index) draw_str(0, 24+i*10, "> ");
                draw_str(12, 24+i*10, et_templates[real_idx]);
                if (htool_api_is_evil_twin_running() && captive_portal_task_args.cp_index == real_idx) {
                    draw_str(90, 24+i*10, "RUN");
                }
            }
            sh1106_update();
        }
        else if (ui_mode == BLE_MENU) {
            clear(); draw_str(0, 0, "BLE Adv:");
            for (int i=0; i<BLE_COUNT; i++) {
                if(i==menu_index) draw_str(0, 12+i*10, "> ");
                draw_str(12, 12+i*10, ble_modes[i]);
                if (htool_api_ble_adv_running() && ble_preset == i) draw_str(90, 12+i*10, "RUN");
            }
            sh1106_update();
        }
        else if (ui_mode == TV_B_GONE_MENU) {
            clear(); draw_str(0, 0, "TV-B-Gone (IR)");
            
            if (htool_api_is_ir_attack_running()) {
                draw_str(30, 25, "ATTACKING!");
                draw_str(10, 45, "Press OK to Stop");
            } else {
                for (int i=0; i<IR_CAT_COUNT; i++) {
                    if (i==menu_index) draw_str(0, 12+i*10, "> ");
                    draw_str(12, 12+i*10, ir_categories[i]);
                }
            }
            sh1106_update();
        }

        // --- BUTTON HANDLING ---
        if (read_button(PIN_BTN_OK)) {
            vTaskDelay(pdMS_TO_TICKS(150));
            if (ui_mode == MAIN_MENU) {
                switch(menu_index) {
                    case 0: scan_selected = 0; start_nonblocking_scan(); ui_mode = SCAN_RESULTS_MENU; pending_op = PENDING_NONE; break;
                    case 1: 
                        if (htool_api_is_deauther_running()) {
                            htool_api_stop_deauther(); show_action_msg("Deauth", "Stopped");
                        } else {
                            scan_selected = 0; start_nonblocking_scan(); ui_mode = SCAN_RESULTS_MENU; pending_op = PENDING_NONE; 
                        }
                        break;
                    case 2: ui_mode = BEACON_MENU; menu_index = beacon_index; break;
                    case 3: ui_mode = CAPTIVE_MENU; menu_index = 0; break;
                    case 4: scan_selected = 0; start_nonblocking_scan(); ui_mode = SCAN_RESULTS_MENU; pending_op = PENDING_EVIL_TWIN; break;
                    case 5: stop_ble_monitor(); htool_api_ble_init(); ui_mode = BLE_MENU; menu_index = ble_preset; break;
                    case 6: ui_mode = TV_B_GONE_MENU; menu_index = 0; break;
                }
            }
            else if (ui_mode == BEACON_MENU) {
                if (htool_api_is_beacon_spammer_running() && beacon_index == menu_index) {
                    htool_api_stop_beacon_spammer();
                } else {
                    htool_api_stop_beacon_spammer(); 
                    beacon_task_args.beacon_index = menu_index;
                    menu_cnt = global_scans_count; 
                    beacon_index = menu_index; nvs_save_settings();
                    htool_api_start_beacon_spammer(menu_index);
                }
            }
            else if (ui_mode == CAPTIVE_MENU) {
                if (htool_api_is_captive_portal_running() && captive_portal_task_args.cp_index == menu_index && !captive_portal_task_args.is_evil_twin) {
                    htool_api_stop_captive_portal();
                } else {
                    htool_api_stop_captive_portal();
                    htool_api_start_captive_portal((uint8_t)menu_index);
                }
            }
            else if (ui_mode == SCAN_RESULTS_MENU && scan_state == WIFI_SCAN_DONE) {
                if (scan_count_local > 0) {
                    target_ap_index = scan_selected; 
                    if (pending_op == PENDING_EVIL_TWIN) {
                        ui_mode = EVIL_TWIN_CONFIG_MENU; menu_index = 0; pending_op = PENDING_NONE;
                    } else {
                        ui_mode = ATTACK_MENU; menu_index = 0;
                    }
                }
            }
            else if (ui_mode == ATTACK_MENU) {
                int global_idx = 0;
                for(int i=0; i<global_scans_count; i++) {
                    if (strcmp((char*)global_scans[i].ssid, (char*)scan_results_local[target_ap_index].record.ssid) == 0) {
                        global_idx = i;
                        break;
                    }
                }

                if (menu_index == 0) htool_api_send_deauth_frame((uint8_t)global_idx, true);
                else if (menu_index == 1) htool_api_send_disassociate_frame((uint8_t)global_idx, true);
                else if (menu_index == 2) htool_api_send_deauth_frame((uint8_t)global_idx, false);
                else if (menu_index == 3) {
                    if (htool_api_is_deauther_running()) {
                        htool_api_stop_deauther();
                    } else {
                        menu_cnt = global_idx; 
                        htool_api_start_deauther();
                    }
                }
            }
            else if (ui_mode == EVIL_TWIN_CONFIG_MENU) {
                et_clone_mac = (menu_index == 0); // 0 = Yes, 1 = No
                ui_mode = EVIL_TWIN_MENU; 
                menu_index = 0;
            }
            else if (ui_mode == EVIL_TWIN_MENU) {
                int global_idx = 0;
                for(int i=0; i<global_scans_count; i++) {
                    if (strcmp((char*)global_scans[i].ssid, (char*)scan_results_local[target_ap_index].record.ssid) == 0) {
                        global_idx = i;
                        break;
                    }
                }

                if (htool_api_is_evil_twin_running() && captive_portal_task_args.cp_index == menu_index) {
                    htool_api_stop_evil_twin();
                } else {
                    htool_api_stop_evil_twin();
                    // Pass the boolean clone preference
                    htool_api_start_evil_twin((uint8_t)global_idx, (uint8_t)menu_index, et_clone_mac);
                }
            }
            else if (ui_mode == BLE_MENU) {
                if (htool_api_ble_adv_running() && ble_preset == menu_index) {
                    htool_api_ble_stop_adv();
                } else {
                    htool_api_set_ble_adv(menu_index); htool_api_ble_start_adv(); 
                    ble_preset = menu_index; nvs_save_settings();
                }
            }
            else if (ui_mode == TV_B_GONE_MENU) {
                if (htool_api_is_ir_attack_running()) {
                    htool_api_stop_ir_attack();
                } else {
                    // Start attack with current category
                    htool_api_start_ir_attack(menu_index);
                }
            }
        }

        if (read_button(PIN_BTN_BACK)) {
            vTaskDelay(pdMS_TO_TICKS(150));
            if (ui_mode == ATTACK_MENU) ui_mode = SCAN_RESULTS_MENU;
            else if (ui_mode == SCAN_RESULTS_MENU) ui_mode = MAIN_MENU;
            else if (ui_mode == BEACON_MENU) { htool_api_stop_beacon_spammer(); ui_mode = MAIN_MENU; }
            else if (ui_mode == CAPTIVE_MENU) { htool_api_stop_captive_portal(); ui_mode = MAIN_MENU; }
            else if (ui_mode == EVIL_TWIN_CONFIG_MENU) { ui_mode = SCAN_RESULTS_MENU; }
            else if (ui_mode == EVIL_TWIN_MENU) { 
                if (htool_api_is_evil_twin_running()) {
                     htool_api_stop_evil_twin(); 
                }
                ui_mode = EVIL_TWIN_CONFIG_MENU; // Go back to config, not scan results
            }
            else if (ui_mode == BLE_MENU) {
                htool_api_ble_stop_adv(); htool_api_ble_deinit(); init_ble_monitor();
                ui_mode = MAIN_MENU;
            }
            else if (ui_mode == TV_B_GONE_MENU) {
                htool_api_stop_ir_attack(); // Safety stop when leaving menu
                ui_mode = MAIN_MENU;
            }
            else ui_mode = MAIN_MENU;
            pending_op = PENDING_NONE;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// -------------------------------------------------------------
// MAIN APP
// -------------------------------------------------------------
static void initialize_esp_modules(void) {
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
}

void app_main(void) {
    ESP_LOGI(TAG, "Start HackingTool (fixed)");
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    }
    initialize_esp_modules();
    i2c_init(); sh1106_init();
    
    // NEW GPIO CONFIG for 4 Buttons
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE, 
        .mode = GPIO_MODE_INPUT,
        // Bitmask for all 4 buttons (32, 33, 25, 26)
        .pin_bit_mask = (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN) | (1ULL << PIN_BTN_OK) | (1ULL << PIN_BTN_BACK),
        .pull_down_en = GPIO_PULLDOWN_DISABLE, 
        .pull_up_en = GPIO_PULLUP_ENABLE // Enable internal pull-ups
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    xTaskCreate(ui_task, "ui_task", 8 * 1024, NULL, tskIDLE_PRIORITY + 2, NULL);

    htool_netman_do_nothing(); 
    htool_api_init();
    htool_wifi_start();
    htool_uart_cli_start();
    
    ESP_LOGI(TAG, "Startup completed");
}