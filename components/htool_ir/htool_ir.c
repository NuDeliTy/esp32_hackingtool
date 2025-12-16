/*
 * htool_ir.c - TV-B-Gone Implementation using ESP32 RMT
 * Includes common Power-Off codes for major brands.
 */
#include "htool_ir.h"
#include "driver/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "htool_ir"

#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV    80             // 80MHz / 80 = 1MHz resolution (1us ticks)

static TaskHandle_t ir_task_handle = NULL;
static bool ir_running = false;

// --- IR PROTOCOL TIMINGS (in microseconds) ---
// NEC
#define NEC_HEADER_HIGH 9000
#define NEC_HEADER_LOW  4500
#define NEC_BIT_ONE_HIGH 560
#define NEC_BIT_ONE_LOW  1690
#define NEC_BIT_ZERO_HIGH 560
#define NEC_BIT_ZERO_LOW  560

// Sony (12-bit)
#define SONY_HEADER_HIGH 2400
#define SONY_HEADER_LOW  600
#define SONY_BIT_ONE_HIGH 1200
#define SONY_BIT_ONE_LOW  600
#define SONY_BIT_ZERO_HIGH 600
#define SONY_BIT_ZERO_LOW  600

// Raw Item Builder Helpers
static void add_pulse(rmt_item32_t *items, int *index, uint16_t high_us, uint16_t low_us) {
    items[*index].duration0 = high_us;
    items[*index].level0 = 1;
    items[*index].duration1 = low_us;
    items[*index].level1 = 0;
    (*index)++;
}

// Send NEC Code (Common for Samsung, LG, Vizio, etc.)
// address: 16-bit, command: 16-bit
static void send_nec(uint16_t address, uint16_t command) {
    int item_count = 1 + 32 + 1; // Header + 32 bits + End pulse
    rmt_item32_t *items = malloc(item_count * sizeof(rmt_item32_t));
    if (!items) return;

    int idx = 0;
    // Header
    add_pulse(items, &idx, NEC_HEADER_HIGH, NEC_HEADER_LOW);

    // Data (Address + Command) - LSB First usually, but standard NEC sends Addr, ~Addr, Cmd, ~Cmd
    // For simplicity in this database, we treat the 32-bit code as raw data stream MSB first
    uint32_t data = (address << 16) | command;
    
    for (int i = 31; i >= 0; i--) {
        if ((data >> i) & 1) {
            add_pulse(items, &idx, NEC_BIT_ONE_HIGH, NEC_BIT_ONE_LOW);
        } else {
            add_pulse(items, &idx, NEC_BIT_ZERO_HIGH, NEC_BIT_ZERO_LOW);
        }
    }
    
    // Stop Bit
    items[idx].duration0 = 560;
    items[idx].level0 = 1;
    items[idx].duration1 = 0; // End
    items[idx].level1 = 0;

    rmt_write_items(RMT_TX_CHANNEL, items, idx + 1, true);
    rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
    free(items);
}

// Send Sony Code (12-bit)
static void send_sony(uint16_t data, int bits) {
    int item_count = 1 + bits; 
    rmt_item32_t *items = malloc(item_count * sizeof(rmt_item32_t));
    if (!items) return;

    int idx = 0;
    // Header
    add_pulse(items, &idx, SONY_HEADER_HIGH, SONY_HEADER_LOW);

    // Data (LSB First for Sony)
    for (int i = 0; i < bits; i++) {
        if ((data >> i) & 1) {
            add_pulse(items, &idx, SONY_BIT_ONE_HIGH, SONY_BIT_ONE_LOW);
        } else {
            add_pulse(items, &idx, SONY_BIT_ZERO_HIGH, SONY_BIT_ZERO_LOW);
        }
    }
    
    rmt_write_items(RMT_TX_CHANNEL, items, idx, true);
    rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
    free(items);
}

// --- ATTACK TASK ---
static void ir_attack_task_func(void *pvParameters) {
    ESP_LOGI(TAG, "Starting TV-B-Gone Sequence");
    
    while (ir_running) {
        // 1. Samsung Power (NEC 0x0707, 0x02FD)
        send_nec(0x0707, 0x02FD); vTaskDelay(pdMS_TO_TICKS(100));
        
        // 2. LG Power (NEC 0x04FB, 0x04FB)
        send_nec(0x04FB, 0x04FB); vTaskDelay(pdMS_TO_TICKS(100));
        
        // 3. Sony Power (12-bit, Data 0x15, Addr 0x1) -> 0xA90 (LSB formatted)
        // Sony Power is Command 21 (0x15), Address 1.
        send_sony(0xA90, 12); vTaskDelay(pdMS_TO_TICKS(100));
        send_sony(0xA90, 12); vTaskDelay(pdMS_TO_TICKS(100)); // Sony requires repeat
        send_sony(0xA90, 12); vTaskDelay(pdMS_TO_TICKS(100)); 

        // 4. NEC Generic Power
        send_nec(0x0000, 0x00FF); vTaskDelay(pdMS_TO_TICKS(100));

        // 5. Vizio (NEC)
        send_nec(0x20DF, 0x10EF); vTaskDelay(pdMS_TO_TICKS(100));

        // Loop delay or Stop after one cycle?
        // Usually TV-B-Gone loops until user stops or runs once. 
        // Let's run once then wait a bit.
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // For continuous mode: remove this break to loop forever
        // break; 
    }
    
    ESP_LOGI(TAG, "IR Sequence Finished");
    ir_running = false;
    ir_task_handle = NULL;
    vTaskDelete(NULL);
}

// --- PUBLIC API ---

void htool_ir_init(void) {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(HTOOL_IR_LED_GPIO, RMT_TX_CHANNEL);
    config.clk_div = RMT_CLK_DIV; 
    // Carrier Frequency for IR (38kHz)
    config.tx_config.carrier_en = true;
    config.tx_config.carrier_freq_hz = 38000;
    config.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    config.tx_config.carrier_duty_percent = 33;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    rmt_config(&config);
    rmt_driver_install(RMT_TX_CHANNEL, 0, 0);
    ESP_LOGI(TAG, "IR Initialized on Pin %d", HTOOL_IR_LED_GPIO);
}

void htool_ir_start_attack(void) {
    if (ir_running) return;
    ir_running = true;
    xTaskCreate(ir_attack_task_func, "ir_task", 4096, NULL, 5, &ir_task_handle);
}

void htool_ir_stop_attack(void) {
    ir_running = false;
    // Task will delete itself
}

bool htool_ir_is_running(void) {
    return ir_running;
}