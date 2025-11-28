/*
Copyright (c) 2023 kl0ibi

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "htool_api.h"
#include "htool_netman.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
// SIMPLE 6x8 FONT
// -------------------------------------------------------------
/* font6x8.h must contain a 256x6 array, e.g.:
   static const uint8_t font6x8[256][6] = {
     {0x00,0x00,0x00,0x00,0x00,0x00}, // 0x00
     ...
   };
   For brevity you can provide a smaller font mapping for printable ASCII only,
   but keep the include name below. Put font6x8.h into main/.

static const uint8_t font6x8[][6] = {

};
*/
// Draw pixel in buffer
static void draw_pixel(int x, int y, int color)
{
    if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
    size_t idx = x + (y / 8) * SH1106_WIDTH;
    if (color)
        sh1106_buffer[idx] |= (1 << (y & 7));
    else
        sh1106_buffer[idx] &= ~(1 << (y & 7));
}

// Draw char
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

// Draw string
static void draw_str(int x, int y, const char *s)
{
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

// Initialize ESP modules used by htool
static void initialize_esp_modules(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
}

// Clear buffer
static void clear(void)
{
    memset(sh1106_buffer, 0, sizeof(sh1106_buffer));
}

// Push buffer to OLED
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
// MENU SYSTEM
// -------------------------------------------------------------
static const char *menu_items[] = {
    "Item 1",
    "Item 2",
    "Item 3",
    "Item 4"
};
static int menu_index = 0;

static void update_menu(void)
{
    clear();
    for (int i = 0; i < 4; i++) {
        if (i == menu_index) {
            draw_str(0, i * 10, "> ");
        }
        draw_str(12, i * 10, menu_items[i]);
    }
    sh1106_update();
}

// UI task: handles display + input polling
static void ui_task(void *arg)
{
    ESP_LOGI(TAG, "UI task started");
    update_menu();

    while (1) {
        int rot = read_rotary();
        if (rot != 0) {
            menu_index += rot;
            if (menu_index < 0) menu_index = 0;
            if (menu_index > 3) menu_index = 3;
            update_menu();
        }

        if (read_button(PIN_ROT_P)) {
            draw_str(0, 52, "EC11 pressed   ");
            sh1106_update();
            vTaskDelay(pdMS_TO_TICKS(220)); // debounce / visual feedback
        }
        if (read_button(PIN_BTN_BAK)) {
            draw_str(0, 52, "Back pressed   ");
            sh1106_update();
            vTaskDelay(pdMS_TO_TICKS(220));
        }
        if (read_button(PIN_BTN_CON)) {
            draw_str(0, 52, "Confirm pressed");
            sh1106_update();
            vTaskDelay(pdMS_TO_TICKS(220));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// -------------------------------------------------------------
// APP MAIN
// -------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "Start HackingTool");

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
    BaseType_t r = xTaskCreate(ui_task, "ui_task", 4 * 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ui_task");
    }

    // keep compatibility with original startup sequence
    htool_netman_do_nothing(); // fix linker error
    htool_api_init();
    htool_api_start();

    ESP_LOGI(TAG, "HackingTool Startup completed");

    // app_main should return; keep the task alive with vTaskDelay if needed
    // but we return to let created tasks run.
}
