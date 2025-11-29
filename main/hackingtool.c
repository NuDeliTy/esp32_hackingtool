/*
hackingtool.c
Full-feature SH1106 menu for HackingTool (WiFi, Deauth, Beacon, Captive Portal, Evil Twin, BLE)

* Non-blocking Wi-Fi scan (background task + scan_state)
* Attack submenu (Deauth / Disassoc / Single / Continuous)
* RSSI + channel shown in results
* NVS store for ble_preset and beacon_idx
* Uses existing htool_api / htool_wifi external functions
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
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "font6x8.h"

static void draw_str(int x, int y, const char *s);
static void clear(void);
static void sh1106_update(void);
static int read_rotary(void);
static int read_button(int pin);


#define TAG "htool_ui"

// -------------------------------------------------------------
// PIN CONFIG
// -------------------------------------------------------------
#define PIN_SDA     21
#define PIN_SCL     22
#define PIN_ROT_A   32
#define PIN_ROT_B   33
#define PIN_ROT_P   25
#define PIN_BTN_BAK 34
#define PIN_BTN_CON 35


// -------------------------------------------------------------
// SH1106 DEFINES
// -------------------------------------------------------------
#define SH1106_ADDR 0x3C
#define SH1106_WIDTH 128
#define SH1106_HEIGHT 64
static uint8_t sh1106_buffer[SH1106_WIDTH * SH1106_HEIGHT / 8];

// Forward
static void ui_task(void *arg);

// -------------------------------------------------------------
// External API data (from htool_api / htool_wifi)
extern wifi_ap_record_t *global_scans;
extern uint8_t global_scans_count;



// captive portal templates (non-evil)
static const char *cp_templates[] = {
    "Google",
    "McDonald's",
    "Facebook",
    "Apple"
};
static const int CP_TEMPLATES_COUNT = sizeof(cp_templates) / sizeof(cp_templates[0]);

// evil twin router templates
static const char *et_templates[] = {
    "General",
    "Huawei",
    "ASUS",
    "Tp-Link",
    "Netgear",
    "o2",
    "FritzBox",
    "Vodafone",
    "Magenta",
    "1&1",
    "A1",
    "Globe",
    "PLDT",
    "AT&T",
    "Swisscom",
    "Verizon"
};
static const int ET_TEMPLATES_COUNT = sizeof(et_templates) / sizeof(et_templates[0]);

// pending action when entering the SCAN_RESULTS_MENU (so we know what to do when AP selected)
typedef enum {
    ACTION_NONE = 0,
    ACTION_DEAUTH,
    ACTION_EVIL_TWIN
} pending_action_t;

static pending_action_t pending_action = ACTION_NONE;

// -----------------------------
// Popup template menus (blocking loops) - returns selected index or -1 for cancel
// -----------------------------
static int captive_portal_template_menu(void)
{
    int idx = 0;
    while (1) {
        clear();
        draw_str(0, 0, "Captive Portal:");
        draw_str(0, 10, "Choose template:");
        for (int i = 0; i < CP_TEMPLATES_COUNT; ++i) {
            int y = 24 + i*8;
            if (i == idx) draw_str(0, y, "> ");
            draw_str(12, y, cp_templates[i]);
        }
        sh1106_update();

        int r = read_rotary();
        if (r > 0) idx++;
        else if (r < 0) idx--;
        if (idx < 0) idx = 0;
        if (idx >= CP_TEMPLATES_COUNT) idx = CP_TEMPLATES_COUNT - 1;

        if (read_button(PIN_ROT_P)) {
            vTaskDelay(pdMS_TO_TICKS(80));
            return idx;
        }
        if (read_button(PIN_BTN_BAK)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static int evil_twin_template_menu(int ap_index)
{
    // ap_index included so we can show SSID if desired
    char buf[40];
    int idx = 0;
    // show target SSID on top if available
    char ssid_local[28] = {0};
    if (global_scans != NULL && ap_index >= 0 && ap_index < (int)global_scans_count) {
        int len = strlen((char*)global_scans[ap_index].ssid);
        if (len > 26) len = 26;
        memcpy(ssid_local, global_scans[ap_index].ssid, len);
        ssid_local[len] = 0;
    }

    while (1) {
        clear();
        draw_str(0, 0, "Evil Twin:");
        if (ssid_local[0]) {
            snprintf(buf, sizeof(buf), "Target: %s", ssid_local);
            draw_str(0, 10, buf);
        } else {
            draw_str(0, 10, "Choose template:");
        }
        for (int i = 0; i < ET_TEMPLATES_COUNT; ++i) {
            int y = 24 + i*8;
            if (i == idx) draw_str(0, y, "> ");
            draw_str(12, y, et_templates[i]);
        }
        sh1106_update();

        int r = read_rotary();
        if (r > 0) idx++;
        else if (r < 0) idx--;
        if (idx < 0) idx = 0;
        if (idx >= ET_TEMPLATES_COUNT) idx = ET_TEMPLATES_COUNT - 1;

        if (read_button(PIN_ROT_P)) {
            vTaskDelay(pdMS_TO_TICKS(80));
            return idx;
        }
        if (read_button(PIN_BTN_BAK)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}



// -------------------------------------------------------------
// Simple SH1106 low-level helpers
static void sh1106_cmd(uint8_t cmd)
{
i2c_cmd_handle_t h = i2c_cmd_link_create();
i2c_master_start(h);
i2c_master_write_byte(h, (SH1106_ADDR << 1) | I2C_MASTER_WRITE, true);
i2c_master_write_byte(h, 0x00, true); // control byte: command
i2c_master_write_byte(h, cmd, true);
i2c_master_stop(h);
i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100));
i2c_cmd_link_delete(h);
}

static void sh1106_data(const uint8_t *data, size_t len)
{
i2c_cmd_handle_t h = i2c_cmd_link_create();
i2c_master_start(h);
i2c_master_write_byte(h, (SH1106_ADDR << 1) | I2C_MASTER_WRITE, true);
i2c_master_write_byte(h, 0x40, true); // control byte: data
i2c_master_write(h, (uint8_t *)data, len, true);
i2c_master_stop(h);
i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100));
i2c_cmd_link_delete(h);
}

static void sh1106_init(void)
{
sh1106_cmd(0xAE); // display OFF
sh1106_cmd(0xD5); sh1106_cmd(0x80);
sh1106_cmd(0xA8); sh1106_cmd(0x3F);
sh1106_cmd(0xD3); sh1106_cmd(0x00);
sh1106_cmd(0x40);
sh1106_cmd(0xAD); sh1106_cmd(0x8B);
sh1106_cmd(0xA1);
sh1106_cmd(0xC8);
sh1106_cmd(0xDA); sh1106_cmd(0x12);
sh1106_cmd(0x81); sh1106_cmd(0x7F);
sh1106_cmd(0xD9); sh1106_cmd(0x22);
sh1106_cmd(0xDB); sh1106_cmd(0x35);
sh1106_cmd(0xA4);
sh1106_cmd(0xA6);
sh1106_cmd(0xAF);
}

static void draw_pixel(int x, int y, int color)
{
if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
size_t idx = x + (y / 8) * SH1106_WIDTH;
if (color)
sh1106_buffer[idx] |= (1 << (y & 7));
else
sh1106_buffer[idx] &= ~(1 << (y & 7));
}

static void draw_char(int x, int y, char ch)
{
const uint8_t *glyph = font6x8[(unsigned char)ch];
for (int i = 0; i < 6; ++i) {
uint8_t col = glyph[i];
for (int j = 0; j < 8; ++j) {
draw_pixel(x + i, y + j, (col >> j) & 1);
}
}
}

static void draw_str(int x, int y, const char *s)
{
while (*s) {
draw_char(x, y, *s++);
x += 6;
}
}

static void clear(void)
{
memset(sh1106_buffer, 0, sizeof(sh1106_buffer));
}

static void sh1106_update(void)
{
for (uint8_t page = 0; page < 8; page++) {
sh1106_cmd(0xB0 + page);
sh1106_cmd(0x02);
sh1106_cmd(0x10);
sh1106_data(&sh1106_buffer[page * SH1106_WIDTH], SH1106_WIDTH);
}
}

// -------------------------------------------------------------
// I2C INIT
// -------------------------------------------------------------
static void i2c_init(void)
{
i2c_config_t cfg = {
.mode = I2C_MODE_MASTER,
.sda_io_num = PIN_SDA,
.sda_pullup_en = GPIO_PULLUP_ENABLE,
.scl_io_num = PIN_SCL,
.scl_pullup_en = GPIO_PULLUP_ENABLE,
.master = {.clk_speed = 400000}
};
ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &cfg));
ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0));
}

// -------------------------------------------------------------
// Rotary + buttons
// -------------------------------------------------------------
static int last_a = 0;

int read_rotary()
{
    // simple button-based “rotary”
    static int last_up = 1;
    static int last_down = 1;

    int up = gpio_get_level(PIN_ROT_A);    // button UP
    int down = gpio_get_level(PIN_ROT_B);  // button DOWN

    int result = 0;

    // detect falling edges (button press)
    if (last_up == 1 && up == 0) {
        result = +1; // UP
    }
    if (last_down == 1 && down == 0) {
        result = -1; // DOWN
    }

    last_up = up;
    last_down = down;

    return result;
}


// Normal EC11 step-by-step rotary decoder
// -------------------------------------------------------------
// Rotary
// -------------------------------------------------------------
// Better rotary decoder with debouncing for EC11

/*
static uint8_t last_state = 0;
static uint32_t last_time = 0;

int read_rotary()
{
    int A = gpio_get_level(PIN_ROT_A);
    int B = gpio_get_level(PIN_ROT_B);
    uint8_t s = (A << 1) | B;

    uint32_t now = esp_timer_get_time(); // microseconds

    // ignore bounces faster than 2ms
    if (now - last_time < 2000) {
        last_state = s;
        return 0;
    }

    last_time = now;

    static const int8_t q_table[16] = {
        0, -1, +1, 0,
        +1, 0, 0, -1,
        -1, 0, 0, +1,
        0, +1, -1, 0
    };

    int8_t dir = q_table[(last_state << 2) | s];
    last_state = s;
    return dir;
}
*/

static int read_button(int pin)
{
return (gpio_get_level(pin) == 0); // active low -> 1 when pressed
}

// -------------------------------------------------------------
// UI / Menu state
// -------------------------------------------------------------
typedef enum {
MAIN_MENU = 0,
SCAN_RESULTS_MENU,
ATTACK_MENU,
BEACON_MENU,
BEACON_SUBMENU,
CAPTIVE_MENU,
EVIL_TWIN_MENU,
EVIL_TWIN_SUBMENU,
BLE_MENU
} ui_mode_t;

static const char *main_items[] = {
"Scan Networks",
"Deauth (toggle)",
"Beacon Spammer",
"Captive Portal",
"Evil Twin",
"BLE Spoof"
};
static const int MAIN_ITEMS = sizeof(main_items)/sizeof(main_items[0]);

static int menu_index = 0;
static ui_mode_t ui_mode = MAIN_MENU;

// -------------------------------------------------------------
// Non-blocking scan task + shared state
// -------------------------------------------------------------
typedef enum {
WIFI_SCAN_IDLE = 0,
WIFI_SCAN_RUNNING,
WIFI_SCAN_DONE
} wifi_scan_state_t;

static volatile wifi_scan_state_t scan_state = WIFI_SCAN_IDLE;
static wifi_ap_record_t scan_results_local[64]; // local copy (max 64)
static int scan_count_local = 0;

static void wifi_scan_task(void *arg)
{
// Start the scan using the existing API (non-blocking in htool_wifi).
scan_state = WIFI_SCAN_RUNNING;
scan_count_local = 0;


// Ask core API to start an active scan (it sets perform_active_scan).
htool_api_start_active_scan();

// Wait up to ~8 seconds (loop polling global_scans_count that htool_wifi fills)
const TickType_t timeout = pdMS_TO_TICKS(8000);
TickType_t start = xTaskGetTickCount();
while ((xTaskGetTickCount() - start) < timeout) {
    if (global_scans_count > 0 && global_scans != NULL) {
        // copy current results into local array (to avoid races)
        int n = global_scans_count;
        if (n > 64) n = 64;
        for (int i = 0; i < n; ++i) {
            scan_results_local[i] = global_scans[i];
        }
        scan_count_local = n;
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

// If nothing found, copy 0
if (scan_count_local == 0 && global_scans_count > 0 && global_scans != NULL) {
    int n = global_scans_count;
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) scan_results_local[i] = global_scans[i];
    scan_count_local = n;
}

scan_state = WIFI_SCAN_DONE;
vTaskDelete(NULL);


}

// helper to request a scan (starts background task)
static void start_nonblocking_scan(void)
{
if (scan_state == WIFI_SCAN_RUNNING) return; // already running
scan_state = WIFI_SCAN_RUNNING;
xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, NULL);
}

// -------------------------------------------------------------
// Attack submenu + helpers
// -------------------------------------------------------------
typedef enum {
ATTACK_DEAUTH = 0,
ATTACK_DISASSOC,
ATTACK_SINGLE_FRAME,
ATTACK_START_CONTINUOUS
} attack_mode_t;

static const char *attack_items[] = {
"Deauth once",
"Disassoc once",
"Single frame",
"Start continuous"
};

static void start_attack(int attack_kind, int ap_index)
{
ESP_LOGI(TAG, "start_attack: kind=%d idx=%d", attack_kind, ap_index);
if (ap_index < 0) return;
// Try to call real API functions. Use htool_api variants used elsewhere in your code.
if (attack_kind == ATTACK_DEAUTH) {
// send deauth once (as station frames)
htool_api_send_deauth_frame((uint8_t)ap_index, true);
}
else if (attack_kind == ATTACK_DISASSOC) {
htool_api_send_disassociate_frame((uint8_t)ap_index, true);
}
else if (attack_kind == ATTACK_SINGLE_FRAME) {
// single frame as deauth but different control (use deauth packet)
htool_api_send_deauth_frame((uint8_t)ap_index, false);
}
else if (attack_kind == ATTACK_START_CONTINUOUS) {
// start the deauther task that repeatedly deauths the index
htool_api_start_deauther();
}
}

// small helper to show a confirmation message on the screen
static void show_action_msg(const char *line1, const char *line2)
{
clear();
draw_str(0, 0, line1);
if (line2) draw_str(0, 12, line2);
sh1106_update();
}

// -------------------------------------------------------------
// NVS: store BLE preset + beacon index
// -------------------------------------------------------------
static nvs_handle_t nvs_h = 0;
static int ble_preset = 0;
static int beacon_index = 0;

static void nvs_init_settings(void)
{
esp_err_t err = nvs_open("htool", NVS_READWRITE, &nvs_h);
if (err != ESP_OK) {
ESP_LOGW(TAG, "NVS open failed: %d", err);
return;
}
int32_t tmp;
if (nvs_get_i32(nvs_h, "ble_preset", &tmp) == ESP_OK) {
ble_preset = tmp;
}
if (nvs_get_i32(nvs_h, "beacon_idx", &tmp) == ESP_OK) {
beacon_index = tmp;
}
}

static void nvs_save_settings(void)
{
if (!nvs_h) return;
nvs_set_i32(nvs_h, "ble_preset", ble_preset);
nvs_set_i32(nvs_h, "beacon_idx", beacon_index);
nvs_commit(nvs_h);
}

// -------------------------------------------------------------
// UI helpers: scan results display (monochrome, compact)
// -------------------------------------------------------------
static void show_scan_results_screen(int selected)
{
clear();
draw_str(0, 0, "Scan results:");
// If we have results from background task, show those, else fall back to global_scans
int count = (scan_state == WIFI_SCAN_DONE ? scan_count_local : global_scans_count);
wifi_ap_record_t *list = (scan_state == WIFI_SCAN_DONE ? scan_results_local : global_scans);


if (count <= 0 || list == NULL) {
    draw_str(0, 12, "No results");
    sh1106_update();
    return;
}

// Show up to 6 entries (6*8 = 48px)
int start = 0;
if (selected > 3 && count > 6) start = selected - 3;

for (int i = 0; i < 6 && (start + i) < count; i++) {
    int idx = start + i;
    int y = 12 + i*8;
    char line[40] = {0};

    // SSID (max 14 chars) + channel + RSSI
    char ssid[20] = {0};
    int s_len = strlen((char*)list[idx].ssid);
    if (s_len > 14) s_len = 14;
    memcpy(ssid, list[idx].ssid, s_len);
    ssid[s_len] = 0;

    snprintf(line, sizeof(line), "%-14s CH:%02d R:%d", ssid, list[idx].primary, list[idx].rssi);
    if (idx == selected) draw_str(0, y, "> ");
    draw_str(12, y, line);
}
sh1106_update();


}

// Attack submenu UI (blocking inside its own small loop)
static void attack_menu(int ap_index)
{
int attack_idx = 0;
while (1) {
clear();
draw_str(0, 0, "Choose attack:");
char t[32];
int len = strlen((char*)global_scans[ap_index].ssid);
char ssid_local[24] = {0};
if (len > 20) len = 20;
memcpy(ssid_local, global_scans[ap_index].ssid, len);
snprintf(t, sizeof(t), "%s", ssid_local);
draw_str(0, 12, t);


    for (int i = 0; i < 4; ++i) {
        if (i == attack_idx) draw_str(0, 24 + i*8, "> ");
        draw_str(12, 24 + i*8, attack_items[i]);
    }
    sh1106_update();

    int r = read_rotary();
    if (r) {
        attack_idx += r;
        if (attack_idx < 0) attack_idx = 0;
        if (attack_idx > 3) attack_idx = 3;
    }

    if (read_button(PIN_ROT_P)) {
        vTaskDelay(pdMS_TO_TICKS(60));
        // Confirm -> perform attack
        // Some attacks run continuously; give confirmation feedback
        start_attack(attack_idx, ap_index);
        show_action_msg("Attack triggered:", attack_items[attack_idx]);
        vTaskDelay(pdMS_TO_TICKS(800));
        return;
    }

    if (read_button(PIN_BTN_BAK)) {
        vTaskDelay(pdMS_TO_TICKS(120));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(80));
}


}

// -------------------------------------------------------------
// UI task: main menu loop (monochrome SH1106)
// -------------------------------------------------------------
static void ui_task(void *arg)
{
ESP_LOGI(TAG, "UI task started");


// NVS init for settings
nvs_init_settings();

// Initial screen
clear();
draw_str(0, 0, "HackingTool ready");
draw_str(0, 12, "Rotate: select");
draw_str(0, 24, "Press: confirm");
sh1106_update();

vTaskDelay(pdMS_TO_TICKS(500));

int scan_selected = 0;
bool deauther_running = false;
bool beacon_running = false;
bool captive_running = false;
bool evil_running = false;
bool ble_running = false;

while (1) {

   

    int rot = read_rotary();
    if (rot) {
    if (ui_mode == MAIN_MENU) {
        menu_index += rot;

        if (menu_index < 0)
            menu_index = 0;
        else if (menu_index >= MAIN_ITEMS)
            menu_index = MAIN_ITEMS - 1;

    } else if (ui_mode == SCAN_RESULTS_MENU) {
        scan_selected += rot;

        int maxcount = (scan_state == WIFI_SCAN_DONE ? scan_count_local : global_scans_count);
        if (maxcount <= 0) maxcount = 0;

        if (scan_selected < 0)
            scan_selected = 0;
        else if (scan_selected >= maxcount)
            scan_selected = maxcount - 1;
    }
}


    // Show screen based on ui_mode
    if (ui_mode == MAIN_MENU) {
        clear();
        draw_str(0, 0, "HackingTool (main)");
        for (int i = 0; i < MAIN_ITEMS; ++i) {
            int y = 12 + i*8;
            if (i == menu_index) draw_str(0, y, "> ");
            draw_str(12, y, main_items[i]);
        }
        sh1106_update();
    } else if (ui_mode == SCAN_RESULTS_MENU) {
        if (scan_state == WIFI_SCAN_RUNNING) {
            // animated scanning
            static int dots = 0;
            clear();
            draw_str(0, 0, "Scanning WiFi...");
            char b[8];
            snprintf(b, sizeof(b), "%.*s", dots, "...");
            draw_str(0, 12, b);
            dots = (dots + 1) % 4;
            // show any partial results
            int partial = (scan_state == WIFI_SCAN_DONE ? scan_count_local : global_scans_count);
            if (partial > 0) {
                // show first 3 SSIDs quickly
                int show = partial;
                if (show > 3) show = 3;
                for (int i = 0; i < show; ++i) {
                    char ss[20] = {0};
                    int len = strlen((char*)global_scans[i].ssid);
                    if (len > 16) len = 16;
                    memcpy(ss, global_scans[i].ssid, len);
                    char line[32];
                    snprintf(line, sizeof(line), "%s R:%d", ss, global_scans[i].rssi);
                    draw_str(0, 24 + i*8, line);
                }
            }
            sh1106_update();
        } else if (scan_state == WIFI_SCAN_DONE) {
            // show final results from local copy
            show_scan_results_screen(scan_selected);
        } else {
            // Idle - no scan requested yet: show hint
            clear();
            draw_str(0, 0, "Scan networks");
            draw_str(0, 12, "Press OK to start");
            sh1106_update();
        }
    }

    // Confirm button
    if (read_button(PIN_ROT_P)) {
        vTaskDelay(pdMS_TO_TICKS(80));
        if (ui_mode == MAIN_MENU) {
            switch (menu_index) {
                case 0: // Scan Networks
                    // Start non-blocking scan
                    scan_selected = 0;
                    scan_state = WIFI_SCAN_RUNNING;
                    start_nonblocking_scan();
                    ui_mode = SCAN_RESULTS_MENU;
                    break;

                case 1: // Deauth toggle: open deauth selector (we use scan results)
                    // We'll re-use scan results: start scan then open SCAN_RESULTS_MENU
                    scan_selected = 0;
                    scan_state = WIFI_SCAN_RUNNING;
                    start_nonblocking_scan();
                    ui_mode = SCAN_RESULTS_MENU;
                    break;

                case 2: // Beacon Spammer - open simple control
                    // toggle beacon spammer running via API; uses existing index from NVS if needed
                    if (!htool_api_is_beacon_spammer_running()) {
                        htool_api_start_beacon_spammer(beacon_index);
                        beacon_running = true;
                        show_action_msg("Beacon spam started", NULL);
                    } else {
                        htool_api_stop_beacon_spammer();
                        beacon_running = false;
                        show_action_msg("Beacon spam stopped", NULL);
                    }
                    vTaskDelay(pdMS_TO_TICKS(600));
                    break;

               case 3: {
    // Open template menu and start chosen portal immediately
    int sel = captive_portal_template_menu();
    if (sel >= 0) {
        // start selected captive portal template
        htool_api_start_captive_portal((uint8_t)sel);
        captive_running = true;
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "CP started: %s", cp_templates[sel]);
        show_action_msg(tbuf, NULL);
    } else {
        show_action_msg("Captive: canceled", NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    break;
}

                case 4: {
    // Start scan then go to SCAN_RESULTS_MENU and let user pick AP,
    // then choose template for evil twin
    scan_selected = 0;
    scan_state = WIFI_SCAN_RUNNING;
    pending_action = ACTION_EVIL_TWIN;
    start_nonblocking_scan();
    ui_mode = SCAN_RESULTS_MENU;
    break;
}

                case 5: // BLE Spoof - open BLE menu (simple)
                    htool_api_ble_init();
                    ui_mode = BLE_MENU;
                    menu_index = 0;
                    break;

                default:
                    break;
            }
        } else if (ui_mode == SCAN_RESULTS_MENU) {
            // If scan done -> present attack submenu for selected AP
            if (scan_state == WIFI_SCAN_DONE || global_scans_count > 0) {
    int maxcount = (scan_state == WIFI_SCAN_DONE ? scan_count_local : global_scans_count);
    if (maxcount <= 0) {
        show_action_msg("No AP to select", NULL);
    } else {
        int ap_idx = scan_selected;
        if (ap_idx >= maxcount) ap_idx = maxcount - 1;

        if (pending_action == ACTION_DEAUTH) {
            // original behaviour: open attack menu
            attack_menu(ap_idx);
        } else if (pending_action == ACTION_EVIL_TWIN) {
            // choose template and start evil twin immediately
            int tpl = evil_twin_template_menu(ap_idx);
            if (tpl >= 0) {
                // Calls into htool_api to start evil twin (uses captive_portal mechanism internally)
                htool_api_start_evil_twin((uint8_t)ap_idx, (uint8_t)tpl);
                evil_running = true;
                char buf2[48];
                snprintf(buf2, sizeof(buf2), "EvilTwin: %s / %s", (char*)global_scans[ap_idx].ssid, et_templates[tpl]);
                show_action_msg("Evil Twin started", buf2);
            } else {
                show_action_msg("EvilTwin: canceled", NULL);
            }
        } else {
            // fallback -> attack menu (keeps original behaviour)
            attack_menu(ap_idx);
        }
        // reset pending action
        pending_action = ACTION_NONE;
    }
} else {
    show_action_msg("Scan in progress", "Wait");
    vTaskDelay(pdMS_TO_TICKS(400));
}
        } else if (ui_mode == BLE_MENU) {
            // Toggle BLE adv with selected preset
            if (htool_api_ble_adv_running()) {
                htool_api_ble_stop_adv();
                ble_running = false;
                show_action_msg("BLE adv stopped", NULL);
            }
            else {
                // choose preset by menu_index: 0..39 possible -> map to API accepted index
                uint8_t preset = (menu_index == 0 ? 39 : menu_index - 1);
                htool_api_set_ble_adv(preset);
                htool_api_ble_start_adv();
                ble_running = true;
                ble_preset = preset;
                nvs_save_settings();
                show_action_msg("BLE adv started", NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        // debounced delay
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    // Back button handling
    if (read_button(PIN_BTN_BAK)) {
        vTaskDelay(pdMS_TO_TICKS(80));
        if (ui_mode == MAIN_MENU) {
            // do nothing (already at top)
            show_action_msg("Main menu", NULL);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else if (ui_mode == SCAN_RESULTS_MENU) {
            ui_mode = MAIN_MENU;
            menu_index = 0;
            scan_state = WIFI_SCAN_IDLE;
            sh1106_update();
        } else if (ui_mode == BLE_MENU) {
            if (htool_api_ble_adv_running()) {
                htool_api_ble_stop_adv();
            }
            htool_api_ble_deinit();
            ui_mode = MAIN_MENU;
            menu_index = 0;
            vTaskDelay(pdMS_TO_TICKS(300));
        } else { // fallback: go to main
            ui_mode = MAIN_MENU;
            menu_index = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // Small UI tick interval
    vTaskDelay(pdMS_TO_TICKS(100));
}


}

// -------------------------------------------------------------
// Initialize ESP modules used by htool
static void initialize_esp_modules(void)
{
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(esp_netif_init());
}

// -------------------------------------------------------------
// App main
void app_main(void)
{
ESP_LOGI(TAG, "Start HackingTool (oled menu) - full feature");




// NVS init for BLE / beacon saving
esp_err_t r = nvs_flash_init();
if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
}

initialize_esp_modules();

// start display / I2C
i2c_init();
sh1106_init();

// configure GPIO inputs with internal pullups (buttons/encoder)
gpio_config_t io_conf = {
    .intr_type = GPIO_INTR_DISABLE,
    .mode = GPIO_MODE_INPUT,
    .pin_bit_mask = (1ULL << PIN_ROT_A) | (1ULL << PIN_ROT_B) |
                    (1ULL << PIN_ROT_P) | (1ULL << PIN_BTN_BAK) |
                    (1ULL << PIN_BTN_CON),
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .pull_up_en = GPIO_PULLUP_ENABLE
};
ESP_ERROR_CHECK(gpio_config(&io_conf));

// start UI task
BaseType_t t = xTaskCreate(ui_task, "ui_task", 8 * 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
if (t != pdPASS) {
    ESP_LOGE(TAG, "Failed to create ui_task");
}

// keep compatibility with original startup sequence
htool_netman_do_nothing(); // keep link happy
htool_api_init();
htool_api_start();

ESP_LOGI(TAG, "HackingTool Startup completed");


}
