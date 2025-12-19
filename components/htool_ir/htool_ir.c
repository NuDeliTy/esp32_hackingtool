/*
 * htool_ir.c - TV-B-Gone with Categories
 * Support for TV, Projectors, and Audio via NEC/Sony/RC5 protocols.
 */
#include "htool_ir.h"
#include "driver/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "htool_ir"
#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV    80 // 1us tick

static TaskHandle_t ir_task_handle = NULL;
static bool ir_running = false;
static int current_category = 0; // 0=TV, 1=Proj, 2=Audio, 3=All

// --- UTILS ---
static void set_carrier(uint32_t freq_hz) {
    rmt_set_tx_carrier(RMT_TX_CHANNEL, true, freq_hz, 33, RMT_CARRIER_LEVEL_HIGH);
}

static void add_pulse(rmt_item32_t *items, int *idx, uint16_t mark, uint16_t space) {
    items[*idx].duration0 = mark;
    items[*idx].level0 = 1;
    items[*idx].duration1 = space;
    items[*idx].level1 = 0;
    (*idx)++;
}

// --- PROTOCOLS ---

// 1. NEC (38kHz)
void send_nec(uint32_t data) {
    set_carrier(38000);
    rmt_item32_t *items = malloc(sizeof(rmt_item32_t) * 34); 
    int idx = 0;
    add_pulse(items, &idx, 9000, 4500); // Header
    for (int i = 31; i >= 0; i--) {
        if ((data >> i) & 1) add_pulse(items, &idx, 560, 1690); 
        else add_pulse(items, &idx, 560, 560);
    }
    add_pulse(items, &idx, 560, 0); // Stop
    rmt_write_items(RMT_TX_CHANNEL, items, idx, true);
    rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
    free(items);
}

// 2. Sony (40kHz) - Sends 3 repeats
void send_sony(uint16_t data, int bits) {
    set_carrier(40000);
    rmt_item32_t *items = malloc(sizeof(rmt_item32_t) * (bits + 1));
    int idx = 0;
    add_pulse(items, &idx, 2400, 600); // Header
    for (int i = 0; i < bits; i++) {
        if ((data >> i) & 1) add_pulse(items, &idx, 1200, 600);
        else add_pulse(items, &idx, 600, 600);
    }
    for (int r = 0; r < 3; r++) {
        rmt_write_items(RMT_TX_CHANNEL, items, idx, true);
        rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
        ets_delay_us(25000);
    }
    free(items);
}

// --- DATABASES ---

typedef enum { PROTO_NEC, PROTO_SONY_12, PROTO_SONY_15, PROTO_SONY_20 } ir_proto_t;

typedef struct {
    ir_proto_t proto;
    uint32_t code;
    const char* name;
} power_code_t;

// 1. TVs (The heavy hitters)
static const power_code_t tv_codes[] = {
    {PROTO_NEC, 0xE0E040BF, "Samsung"},
    {PROTO_NEC, 0x20DF10EF, "LG/Vizio"},
    {PROTO_SONY_12, 0xA90,  "Sony TV (12b)"},
    {PROTO_NEC, 0x000000FF, "NEC Generic"}, 
    {PROTO_NEC, 0x10E03FC0, "Philips"},
    {PROTO_SONY_12, 0x95,   "Sony Old"}, // 0xA90 flipped?
    {PROTO_NEC, 0x02FD0707, "Toshiba"},
    {PROTO_NEC, 0x807F3CC3, "Sharp"},
    {PROTO_NEC, 0x20DFD02F, "LG Alt"},
    {PROTO_SONY_15, 0xA90,  "Sony TV (15b)"},
};

// 2. Projectors (Common Classroom/Office Brands)
static const power_code_t proj_codes[] = {
    {PROTO_NEC, 0xC1AA09F6, "Epson Proj"},
    {PROTO_NEC, 0x00FFE01F, "BenQ Proj"},
    {PROTO_NEC, 0x847402FD, "Dell Proj"},
    {PROTO_NEC, 0x3088CD32, "Optoma Proj"},
    {PROTO_NEC, 0x38863BC4, "ViewSonic"},
    {PROTO_NEC, 0x738C00FF, "Hitachi Proj"},
    {PROTO_NEC, 0xC53AB24D, "Acer Proj"},
};

// 3. Audio / Misc
static const power_code_t audio_codes[] = {
    {PROTO_SONY_12, 0x540C, "Sony Audio"}, // Common Receiver Power
    {PROTO_SONY_15, 0x540C, "Sony Audio 15"},
    {PROTO_NEC, 0x857AE0E0, "Yamaha"}, // Often partial NEC
    {PROTO_NEC, 0x5EA1B847, "Onkyo"},
    {PROTO_NEC, 0x4B36D32C, "Denon"},
    // Soundbars often use TV codes (Samsung/LG/Vizio) which are in the TV list
};

// --- ATTACK LOGIC ---

static void play_list(const power_code_t* list, int count) {
    for (int i = 0; i < count; i++) {
        if (!ir_running) return;
        ESP_LOGI(TAG, "Sending: %s", list[i].name);
        
        switch (list[i].proto) {
            case PROTO_NEC: send_nec(list[i].code); break;
            case PROTO_SONY_12: send_sony((uint16_t)list[i].code, 12); break;
            case PROTO_SONY_15: send_sony((uint16_t)list[i].code, 15); break;
            case PROTO_SONY_20: send_sony((uint16_t)list[i].code, 20); break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void ir_attack_task_func(void *pvParameters) {
    ESP_LOGI(TAG, "Starting IR Attack Category: %d", current_category);

    while (ir_running) {
        if (current_category == 0 || current_category == 3) {
            ESP_LOGI(TAG, ">>> TV Codes <<<");
            play_list(tv_codes, sizeof(tv_codes)/sizeof(tv_codes[0]));
        }
        
        if (current_category == 1 || current_category == 3) {
            ESP_LOGI(TAG, ">>> Projector Codes <<<");
            play_list(proj_codes, sizeof(proj_codes)/sizeof(proj_codes[0]));
        }

        if (current_category == 2 || current_category == 3) {
            ESP_LOGI(TAG, ">>> Audio Codes <<<");
            play_list(audio_codes, sizeof(audio_codes)/sizeof(audio_codes[0]));
        }

        // Pause
        ESP_LOGI(TAG, "Cycle Done. Waiting...");
        for (int k = 0; k < 50; k++) {
            if (!ir_running) goto exit_loop;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

exit_loop:
    ESP_LOGI(TAG, "IR Stopped");
    set_carrier(0);
    rmt_set_tx_carrier(RMT_TX_CHANNEL, false, 0, 0, RMT_CARRIER_LEVEL_HIGH);
    ir_running = false;
    ir_task_handle = NULL;
    vTaskDelete(NULL);
}

// --- API ---

void htool_ir_init(void) {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(HTOOL_IR_LED_GPIO, RMT_TX_CHANNEL);
    config.clk_div = RMT_CLK_DIV;
    config.tx_config.carrier_en = true;
    config.tx_config.carrier_freq_hz = 38000;
    config.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    config.tx_config.carrier_duty_percent = 33;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    rmt_config(&config);
    rmt_driver_install(RMT_TX_CHANNEL, 0, 0);
}

// Updated to accept category (0=TV, 1=Proj, 2=Audio, 3=All)
void htool_ir_start_attack(int category) {
    if (ir_running) return;
    current_category = category;
    ir_running = true;
    xTaskCreate(ir_attack_task_func, "ir_task", 4096, NULL, 5, &ir_task_handle);
}

void htool_ir_stop_attack(void) {
    ir_running = false;
}

bool htool_ir_is_running(void) {
    return ir_running;
}