/* hackingtool.c
Minimal menu + action wiring for EC11 + SH1106 OLED
Uses functions from htool_api / htool_netman / htool_ble etc.
Patched: non-blocking Wi-Fi scan task, scan UI animation, RSSI/channel display,
safe attack submenu (placeholders), NVS store for BLE/beacon index.
*/

#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
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
#include "nvs_flash.h"
#include "nvs.h"
#include "font6x8.h"

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

// tag
static const char *TAG = "htool_ui";

// Forward
static void ui_task(void *arg);

// -------------------------------------------------------------
// External API data (from htool_api.h)
extern wifi_ap_record_t *global_scans;
extern uint8_t global_scans_count;

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
.master = {
.clk_speed = 400000
}
};
ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &cfg));
ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0));
}

// -------------------------------------------------------------
// SH1106 LOW-LEVEL
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// SH1106 INIT SEQUENCE
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// SIMPLE 6x8 FONT helpers (font6x8.h must define font6x8)
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
sh1106_cmd(0x00);
sh1106_cmd(0x10);
sh1106_data(&sh1106_buffer[page * SH1106_WIDTH], SH1106_WIDTH);
}
}

// -------------------------------------------------------------
// ROTARY ENCODER / BUTTON HANDLERS
// -------------------------------------------------------------
static int last_a = 0;
static int read_rotary(void)
{
int a = gpio_get_level(PIN_ROT_A);
int b = gpio_get_level(PIN_ROT_B);


if (a != last_a) {
    last_a = a;
    if (a == b) return +1;  // clockwise
    else        return -1;  // counter
}
return 0;


}

static int read_button(int pin)
{
return (gpio_get_level(pin) == 0); // active low -> return 1 when pressed
}

// -------------------------------------------------------------
// MENU SYSTEM + items
// -------------------------------------------------------------
typedef enum {
MAIN_MENU = 0,
SCAN_RESULTS_MENU,
BLE_MENU,
BEACON_MENU,
} ui_mode_t;

static const char *main_items[] = {
"Scan Networks",
"Deauth (toggle)",
"Beacon Spammer",
"Captive Portal",
"Evil Twin",
"BLE Spoof"
};
static const int MAIN_ITEMS = sizeof(main_items) / sizeof(main_items[0]);

static int menu_index = 0;
static ui_mode_t ui_mode = MAIN_MENU;

// -------------------------------
// Non-blocking scan state + local result copy
typedef enum {
WIFI_SCAN_IDLE,
WIFI_SCAN_RUNNING,
WIFI_SCAN_DONE
} wifi_scan_state_t;

static wifi_scan_state_t scan_state = WIFI_SCAN_IDLE;
static wifi_ap_record_t scan_results[64];
static uint16_t scan_count = 0;

// -------------------------------
// NVS keys + variables
static nvs_handle_t nvs_h;
static int32_t nvs_ble_preset = 0;
static int32_t nvs_beacon_idx = 0;

// helper: draw main menu
static void update_main_menu(void)
{
clear();
draw_str(0, 0, "HackingTool (enc)");
for (int i = 0; i < MAIN_ITEMS; i++) {
int y = 12 + i * 8;
if (i == menu_index) draw_str(0, y, "> ");
draw_str(12, y, main_items[i]);
}
sh1106_update();
}

// helper: draw a short status message
static void show_status(const char *line1, const char *line2)
{
clear();
draw_str(0, 0, line1);
if (line2) draw_str(0, 12, line2);
sh1106_update();
}

// -------------------------------
// Safe placeholder for starting "attacks" (NO real attack calls here).
// This only logs & displays the intended action. If you later want to
// re-enable real calls, insert them where indicated in comments.
static void start_attack_placeholder(int attack_kind, int ap_index)
{
char buf[40];
const char *what = "UNKNOWN";
if (attack_kind == 0) what = "Deauth once";
else if (attack_kind == 1) what = "Disassoc once";
else if (attack_kind == 2) what = "Single frame";
else if (attack_kind == 3) what = "Start continuous";


snprintf(buf, sizeof(buf), "%s -> idx %d", what, ap_index);
ESP_LOGW(TAG, "ATTACK_PLACEHOLDER: %s", buf);

// Show a clear confirmation message on the display.
clear();
draw_str(0, 0, "ATTACK (placeholder)");
draw_str(0, 12, what);
char t[24];
snprintf(t, sizeof(t), "AP idx: %d", ap_index);
draw_str(0, 24, t);
draw_str(0, 40, "No frames sent");
sh1106_update();

// If you later decide to re-enable actual calls, example places:
// if (attack_kind == 0) htool_api_send_deauth_frame(ap_index, true);
// if (attack_kind == 1) htool_api_send_disassociate_frame(ap_index, true);
// if (attack_kind == 3) htool_api_start_deauther();


}

// -------------------------------
// Attack submenu UI
static const char *attack_items[] = {
"Deauth once",
"Disassociate",
"Single frame",
"Start continuous"
};

static void attack_menu(int ap_index)
{
int attack_index = 0;
while (1) {
clear();
// show SSID (truncated)
char ssid_buf[28] = {0};
int len = strlen((char*)scan_results[ap_index].ssid);
if (len > 26) len = 26;
memcpy(ssid_buf, scan_results[ap_index].ssid, len);
draw_str(0, 0, ssid_buf);


    for (int i = 0; i < 4; i++) {
        int y = 12 + i * 10;
        if (i == attack_index) draw_str(0, y, "> ");
        draw_str(12, y, attack_items[i]);
    }
    sh1106_update();

    int rot = read_rotary();
    if (rot) {
        attack_index += rot;
        if (attack_index < 0) attack_index = 0;
        if (attack_index > 3) attack_index = 3;
    }

    if (read_button(PIN_ROT_P)) {
        vTaskDelay(pdMS_TO_TICKS(120)); // debounce
        // confirmation screen
        clear();
        draw_str(0, 0, "Confirm action?");
        draw_str(0, 12, attack_items[attack_index]);
        draw_str(0, 24, "Press OK to confirm");
        draw_str(0, 36, "Back to cancel");
        sh1106_update();

        // wait for button press
        bool done = false;
        while (!done) {
            if (read_button(PIN_ROT_P)) {
                vTaskDelay(pdMS_TO_TICKS(120));
                // safe placeholder (does NOT send frames)
                start_attack_placeholder(attack_index, ap_index);
                vTaskDelay(pdMS_TO_TICKS(800));
                return;
            }
            if (read_button(PIN_BTN_BAK)) {
                vTaskDelay(pdMS_TO_TICKS(120));
                done = true; // back to attack menu
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    if (read_button(PIN_BTN_BAK)) {
        vTaskDelay(pdMS_TO_TICKS(120));
        return; // back to scan results
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}


}

// -------------------------------------------------------------
// show scan results (up to fits on screen) - uses local scan_results
static void show_scan_results(int sel_index)
{
clear();
draw_str(0, 0, "Scan results:");
if (scan_count == 0) {
draw_str(0, 12, "No results");
sh1106_update();
return;
}
// show up to 6 lines (fits 6*8=48px)
int start = 0;
if (sel_index > 3 && scan_count > 6) start = sel_index - 3;
for (int i = 0; i < 6 && (start + i) < scan_count; i++) {
int y = 12 + i * 8;
int idx = start + i;
char buf[40] = {0};
// SSID (max 14 chars), CH and RSSI
int len = strlen((char*)scan_results[idx].ssid);
if (len > 14) len = 14;
memcpy(buf, scan_results[idx].ssid, len);
buf[len] = 0;
char tail[24];
snprintf(tail, sizeof(tail), " CH:%02d R:%+d", scan_results[idx].primary, scan_results[idx].rssi);
if (idx == sel_index) draw_str(0, y, "> ");
draw_str(12, y, buf);
draw_str(12 + 6*14, y, tail); // place at right of ssid (14 chars *6px)
}
sh1106_update();
}

// -------------------------------------------------------------
// Background scan task
static void wifi_scan_task(void *arg)
{
scan_state = WIFI_SCAN_RUNNING;


// Start the existing component's active scan (non-blocking)
htool_api_start_active_scan();

// Wait for results to be populated by the wifi component.
// We poll global_scans_count for simplicity (it is updated by htool_wifi code).
const int max_wait_ms = 8000;
int waited = 0;
while (waited < max_wait_ms) {
    if (global_scans_count > 0 && global_scans != NULL) break;
    vTaskDelay(pdMS_TO_TICKS(100));
    waited += 100;
}

// Copy results into local buffer (safe to read global_scans)
scan_count = (global_scans_count > 64) ? 64 : global_scans_count;
if (scan_count > 0 && global_scans != NULL) {
    for (uint16_t i = 0; i < scan_count; ++i) {
        memcpy(&scan_results[i], &global_scans[i], sizeof(wifi_ap_record_t));
        // ensure SSID is null-terminated (wifi_ap_record_t.ssid is 33 bytes typically)
        scan_results[i].ssid[32] = 0;
    }
} else {
    scan_count = 0;
}

scan_state = WIFI_SCAN_DONE;
vTaskDelete(NULL);


}

// -------------------------------------------------------------
// UI task: handles display + input polling and menu actions
static bool deauther_running = false;
static bool beacon_running = false;
static bool captive_running = false;
static bool ble_adv_running_flag = false;

static void save_nvs_settings()
{
esp_err_t err;
if (nvs_h) {
err = nvs_set_i32(nvs_h, "ble_preset", nvs_ble_preset);
if (err != ESP_OK) ESP_LOGW(TAG, "nvs_set_i32(ble_preset) failed: %d", err);
err = nvs_set_i32(nvs_h, "beacon_idx", nvs_beacon_idx);
if (err != ESP_OK) ESP_LOGW(TAG, "nvs_set_i32(beacon_idx) failed: %d", err);
nvs_commit(nvs_h);
}
}

static void load_nvs_settings()
{
esp_err_t err;
// nvs_h already opened in app_main
if (nvs_h) {
err = nvs_get_i32(nvs_h, "ble_preset", &nvs_ble_preset);
if (err == ESP_ERR_NVS_NOT_FOUND) {
nvs_ble_preset = 0;
}
err = nvs_get_i32(nvs_h, "beacon_idx", &nvs_beacon_idx);
if (err == ESP_ERR_NVS_NOT_FOUND) {
nvs_beacon_idx = 0;
}
}
}

static void ui_task(void *arg)
{
ESP_LOGI(TAG, "UI task started");
update_main_menu();


int scan_sel = 0;

while (1) {
    // If a scan is running, show the scanning animation and continue updating UI
    if (scan_state == WIFI_SCAN_RUNNING) {
        clear();
        draw_str(0, 0, "Scanning WiFi...");
        static int dots = 0;
        char buf[8] = {0};
        for (int i = 0; i < dots; ++i) buf[i] = '.';
        buf[dots] = 0;
        draw_str(0, 12, buf);
        dots = (dots + 1) % 4;
        sh1106_update();
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
    }

    // When scan finishes, show results and switch mode
    if (scan_state == WIFI_SCAN_DONE) {
        scan_state = WIFI_SCAN_IDLE; // consume
        scan_sel = 0;
        ui_mode = SCAN_RESULTS_MENU;
        show_scan_results(scan_sel);
    }

    int rot = read_rotary();
    if (rot != 0) {
        if (ui_mode == MAIN_MENU) {
            menu_index += rot;
            if (menu_index < 0) menu_index = 0;
            if (menu_index >= MAIN_ITEMS) menu_index = MAIN_ITEMS - 1;
            update_main_menu();
        } else if (ui_mode == SCAN_RESULTS_MENU) {
            scan_sel += rot;
            if (scan_sel < 0) scan_sel = 0;
            if (scan_sel >= (int)scan_count && scan_count>0) scan_sel = scan_count - 1;
            if (scan_count == 0) scan_sel = 0;
            show_scan_results(scan_sel);
        } else if (ui_mode == BLE_MENU) {
            // rotate to change BLE preset (0..n) - we store in nvs_ble_preset
            nvs_ble_preset += rot;
            if (nvs_ble_preset < 0) nvs_ble_preset = 0;
            if (nvs_ble_preset > 127) nvs_ble_preset = 127;
            clear();
            char t[32];
            snprintf(t, sizeof(t), "BLE preset: %d", nvs_ble_preset);
            draw_str(0, 0, t);
            draw_str(0, 12, "Press OK to toggle");
            sh1106_update();
        }
    }

    // Confirm button pressed -> perform action
    if (read_button(PIN_ROT_P)) {
        vTaskDelay(pdMS_TO_TICKS(60)); // simple debounce
        if (ui_mode == MAIN_MENU) {
            switch (menu_index) {
                case 0: // Scan Networks -> start non-blocking scan task
                {
                    if (scan_state == WIFI_SCAN_RUNNING) {
                        show_status("Scan already", "in progress");
                    } else {
                        scan_state = WIFI_SCAN_RUNNING;
                        // reset local results
                        scan_count = 0;
                        memset(scan_results, 0, sizeof(scan_results));
                        // create background task to trigger component scan and copy results
                        xTaskCreate(wifi_scan_task, "wifi_scan_task", 4 * 1024, NULL, tskIDLE_PRIORITY + 4, NULL);
                    }
                    vTaskDelay(pdMS_TO_TICKS(220));
                    break;
                }
                case 1: // Deauth toggle
                {
                    if (!deauther_running) {
                        // Note: original code calls htool_api_start_deauther()
                        // We keep that behavior (high level toggle) — this does NOT send single frames here.
                        htool_api_start_deauther();
                        deauther_running = true;
                        show_status("Deauther started", "Back to stop");
                    } else {
                        htool_api_stop_deauther();
                        deauther_running = false;
                        show_status("Deauther stopped", NULL);
                    }
                    vTaskDelay(pdMS_TO_TICKS(220));
                    break;
                }
                case 2: // Beacon spammer
                {
                    if (!beacon_running) {
                        // start with last saved beacon index
                        htool_api_start_beacon_spammer((uint8_t)nvs_beacon_idx);
                        beacon_running = true;
                        show_status("Beacon spam started", "Back to stop");
                    } else {
                        htool_api_stop_beacon_spammer();
                        beacon_running = false;
                        show_status("Beacon spam stopped", NULL);
                    }
                    vTaskDelay(pdMS_TO_TICKS(220));
                    break;
                }
                case 3: // Captive portal
                {
                    if (!captive_running) {
                        // cp_index 0 default
                        htool_api_start_captive_portal(0);
                        captive_running = true;
                        show_status("Captive portal on", "Back to stop");
                    } else {
                        htool_api_stop_captive_portal();
                        captive_running = false;
                        show_status("Captive portal off", NULL);
                    }
                    vTaskDelay(pdMS_TO_TICKS(220));
                    break;
                }
                case 4: // Evil Twin
                {
                    // simply start evil twin with ssid_index 0 and cp_index 0 for now
                    htool_api_start_evil_twin(0, 0);
                    show_status("Evil Twin started", "Press Back to stop");
                    vTaskDelay(pdMS_TO_TICKS(220));
                    break;
                }
                case 5: // BLE Spoof - open BLE menu to pick preset
                {
                    ui_mode = BLE_MENU;
                    clear();
                    char t[32];
                    snprintf(t, sizeof(t), "BLE preset: %d", nvs_ble_preset);
                    draw_str(0, 0, "BLE Spoof - pick idx");
                    draw_str(0, 12, t);
                    draw_str(0, 24, "Rotate select");
                    draw_str(0, 36, "Press to start/stop");
                    sh1106_update();
                    vTaskDelay(pdMS_TO_TICKS(220));
                    break;
                }
                default:
                    break;
            }
        } else if (ui_mode == SCAN_RESULTS_MENU) {
            // Open attack submenu for selected AP (safe placeholder)
            if (scan_count > 0) {
                attack_menu(scan_sel);
                // after attack menu returns, redraw results
                show_scan_results(scan_sel);
            }
            vTaskDelay(pdMS_TO_TICKS(220));
        } else if (ui_mode == BLE_MENU) {
            // toggle BLE adv with current nvs_ble_preset
            htool_api_set_ble_adv((uint8_t)nvs_ble_preset);
            if (!htool_api_ble_adv_running()) {
                htool_api_ble_start_adv();
                ble_adv_running_flag = true;
                show_status("BLE adv started", NULL);
            } else {
                htool_api_ble_stop_adv();
                ble_adv_running_flag = false;
                show_status("BLE adv stopped", NULL);
            }
            // save preset
            save_nvs_settings();
            vTaskDelay(pdMS_TO_TICKS(220));
        }
    }

    // Back button handling
    if (read_button(PIN_BTN_BAK)) {
        vTaskDelay(pdMS_TO_TICKS(60)); // debounce
        if (ui_mode == MAIN_MENU) {
            // small feedback
            show_status("Main menu", NULL);
        } else if (ui_mode == SCAN_RESULTS_MENU) {
            ui_mode = MAIN_MENU;
            menu_index = 0;
            update_main_menu();
        } else if (ui_mode == BLE_MENU) {
            // ensure adv stopped when leaving BLE menu (optional)
            if (ble_adv_running_flag) {
                htool_api_ble_stop_adv();
                ble_adv_running_flag = false;
            }
            ui_mode = MAIN_MENU;
            menu_index = 0;
            update_main_menu();
        }
        vTaskDelay(pdMS_TO_TICKS(220));
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}


}

// -------------------------------------------------------------
// Initialize ESP modules used by htool (and NVS)
static void initialize_esp_modules(void)
{
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(esp_netif_init());
}

// -------------------------------------------------------------
// APP MAIN
// -------------------------------------------------------------
void app_main(void)
{
ESP_LOGI(TAG, "Start HackingTool (oled menu)");


// init NVS
esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // erase and retry
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
} else {
    ESP_ERROR_CHECK(err);
}

// open namespace
if (nvs_open("htool", NVS_READWRITE, &nvs_h) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to open NVS namespace 'htool'");
    nvs_h = 0;
} else {
    // load previously saved settings
    load_nvs_settings();
}

initialize_esp_modules();

// start UI: init I2C, sh1106 and gpio, then create ui task
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

// create UI task
BaseType_t r = xTaskCreate(ui_task, "ui_task", 6 * 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
if (r != pdPASS) {
    ESP_LOGE(TAG, "Failed to create ui_task");
}

// keep compatibility with original startup sequence
htool_netman_do_nothing(); // fix linker error
htool_api_init();
htool_api_start();

// If BLE preset was saved, set it (but don't start adv automatically)
if (nvs_ble_preset > 0) {
    htool_api_set_ble_adv((uint8_t)nvs_ble_preset);
}

// if beacon idx saved, keep it for future start_beacon_spammer calls
ESP_LOGI(TAG, "HackingTool Startup completed (ble_preset=%d beacon_idx=%d)", nvs_ble_preset, nvs_beacon_idx);


}
