#ifndef HTOOL_DISPLAY_H
#define HTOOL_DISPLAY_H

#include <stdbool.h>

extern volatile bool scan_started;

bool deauth_all = false; // Keep this as you had it

typedef enum {
    ST_MENU = 0,
    ST_SCAN,
    ST_DEAUTH,
    ST_BEACON,
    ST_C_PORTAL,
    ST_EVIL_TWIN,
    ST_BLE_SPOOF,
    ST_STARTUP,
    ST_BEACON_SUBMENU,
    ST_EVIL_TWIN_SUBMENU,
    ST_BLE_SPOOF_SUBMENU1,
    ST_BLE_SPOOF_SUBMENU2,
    ST_TV_B_GONE  // <--- ADD THIS LINE ONLY
} display_states;

void htool_display_init();

void htool_display_start();
#endif