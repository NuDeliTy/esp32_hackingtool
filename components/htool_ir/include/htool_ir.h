/*
 * htool_ir.h - IR / TV-B-Gone Header
 */
#ifndef HTOOL_IR_H
#define HTOOL_IR_H

#include <stdbool.h>
#include <stdint.h>

// --- CONFIGURATION ---
// Change this pin to wherever you attach your IR LED
#define HTOOL_IR_LED_GPIO 23

// Initialize the RMT driver for IR transmission
void htool_ir_init(void);

// Start the "TV-B-Gone" attack sequence
// category: 0=TV, 1=Projectors, 2=Audio, 3=All
void htool_ir_start_attack(int category);

// Stop the attack sequence
void htool_ir_stop_attack(void);

// Check if the attack is currently running
bool htool_ir_is_running(void);

#endif