/*
 * ProtoDrop
 * Target: Flipper Zero Wi-Fi Dev Board / ESP32-S2
 *
 * Features:
 * - Creates Wi-Fi AP: ProtoDrop
 * - Hosts webpage at http://192.168.4.1
 * - Accepts messages from browser form
 * - Decodes browser text into readable text
 * - Saves messages to SPIFFS flash storage
 * - Shows saved messages at /messages
 * - Wipes messages at /wipe
 * - Provides UART commands for future Flipper app control
 *
 * UART commands:
 * STATUS
 * COUNT
 * WIPE
 * HELP
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"

#include "driver/uart.h"

/* --------------------------------------------------------------------------
 * Basic configuration
 * -------------------------------------------------------------------------- */

#define AP_SSID "ProtoDrop"
#define AP_PASS "protodrop"
#define AP_CHANNEL 6
#define MAX_CLIENTS 4

#define MESSAGE_FILE "/spiffs/messages.txt"

/*
 * Official Flipper Wi-Fi Dev Board UART routing:
 *
 * ESP32-S2 UART0 TXD0 -> Flipper RX
 * ESP32-S2 UART0 RXD0 -> Flipper TX
 *
 * No custom GPIO routing needed.
 * The PCB already routes UART0 correctly.
 */


#define FLIPPER_UART UART_NUM_1
#define UART_TX_PIN 43
#define UART_RX_PIN 44
#define UART_BAUD_RATE 115200
#define UART_BUFFER_SIZE 1024

static const char *TAG = "ProtoDrop";

/* --------------------------------------------------------------------------
 * Web page HTML
 * -------------------------------------------------------------------------- */

static const char *html_page =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<title>ProtoDrop</title>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>"
"body{font-family:monospace;background:#111;color:#eee;padding:20px;}"
".box{border:1px solid #555;padding:15px;border-radius:8px;max-width:650px;}"
"textarea{width:100%;height:120px;background:#222;color:#eee;border:1px solid #555;padding:8px;}"
"button,a{display:inline-block;padding:10px;margin-top:10px;background:#333;color:#eee;border:1px solid #777;text-decoration:none;}"
"</style>"
"</head>"
"<body>"
"<div class='box'>"
"<h1>ProtoDrop</h1>"
"<p>Offline local message drop.</p>"
"<form method='POST' action='/submit'>"
"<textarea name='msg' maxlength='100' placeholder='Leave a message, max 100 characters...'></textarea><br>"
"<p style='font-size:12px;color:#aaa;'>100 character limit</p>"
"<button type='submit'>Submit</button>"
"</form>"
"<br>"
"<a href='/messages'>View Messages</a> "
"<a href='/wipe'>Wipe Messages</a>"
"</div>"
"</body>"
"</html>";

/* --------------------------------------------------------------------------
 * SPIFFS storage
 * -------------------------------------------------------------------------- */

void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS");
        return;
    }

    ESP_LOGI(TAG, "SPIFFS mounted");
}

void save_message(const char *message) {
    FILE *file = fopen(MESSAGE_FILE, "a");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open message file");
        return;
    }

    fprintf(file, "%s\n", message);
    fclose(file);

    ESP_LOGI(TAG, "Message saved");
}

void wipe_messages(void) {
    FILE *file = fopen(MESSAGE_FILE, "w");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to wipe messages");
        return;
    }

    fclose(file);
    ESP_LOGI(TAG, "Messages wiped");
}

/*
 * Reads the latest saved message from SPIFFS.
 *
 * This walks through messages.txt line by line and keeps overwriting
 * latest_message until the final line is reached.
 */
void get_latest_message(char* latest_message, size_t max_len) {
    FILE* file = fopen(MESSAGE_FILE, "r");

    if(file == NULL) {
        snprintf(latest_message, max_len, "MSG: No messages");
        return;
    }

    char line[256];
    latest_message[0] = '\0';

    while(fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        snprintf(latest_message, max_len, "MSG: %s", line);
    }

    fclose(file);

    if(strlen(latest_message) == 0) {
        snprintf(latest_message, max_len, "MSG: No messages");
    }
}

int count_messages(void) {
    FILE *file = fopen(MESSAGE_FILE, "r");

    if (file == NULL) {
        return 0;
    }

    int count = 0;
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        count++;
    }

    fclose(file);
    return count;
}

/*
 * Reads one saved message by number.
 *
 * Message numbers start at 1.
 *
 * Example:
 * GET 1 returns the first saved message.
 * GET 2 returns the second saved message.
 */
void get_message_by_number(int target_number, char* response, size_t max_len) {
    FILE* file = fopen(MESSAGE_FILE, "r");

    if(file == NULL) {
        snprintf(response, max_len, "MSG: No messages");
        return;
    }

    char line[256];
    int current_number = 1;

    while(fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';

        if(current_number == target_number) {
            snprintf(response, max_len, "MSG: %s", line);
            fclose(file);
            return;
        }

        current_number++;
    }

    fclose(file);

    snprintf(response, max_len, "MSG: No messages");
}

/* --------------------------------------------------------------------------
 * URL decoding
 *
 * Browsers send form text like this:
 * msg=Hello+world%21
 *
 * This code turns it into:
 * Hello world!
 * -------------------------------------------------------------------------- */

int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;

    return 0;
}

void url_decode_message(char *decoded, const char *encoded, size_t decoded_size) {
    size_t i = 0;
    size_t j = 0;

    if (strncmp(encoded, "msg=", 4) == 0) {
        encoded += 4;
    }

    while (encoded[i] != '\0' && j < decoded_size - 1) {
        if (encoded[i] == '+') {
            decoded[j++] = ' ';
            i++;
        } else if (
            encoded[i] == '%' &&
            encoded[i + 1] != '\0' &&
            encoded[i + 2] != '\0'
        ) {
            int high = hex_to_int(encoded[i + 1]);
            int low = hex_to_int(encoded[i + 2]);

            decoded[j++] = (char)((high << 4) | low);
            i += 3;
        } else {
            decoded[j++] = encoded[i++];
        }
    }

    decoded[j] = '\0';
}

/* --------------------------------------------------------------------------
 * HTTP route handlers
 * -------------------------------------------------------------------------- */

esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t submit_post_handler(httpd_req_t *req) {
    char raw_buffer[256];
    char clean_message[256];

    int received = httpd_req_recv(req, raw_buffer, sizeof(raw_buffer) - 1);

    if (received > 0) {
        raw_buffer[received] = '\0';

        url_decode_message(clean_message, raw_buffer, sizeof(clean_message));
        /*
        * Hard safety limit.
        * Even if someone bypasses the webpage limit,
        * only save the first 100 characters.
        */
        clean_message[100] = '\0';

        ESP_LOGI(TAG, "Message received: %s", clean_message);

        save_message(clean_message);
    }

    const char *response =
        "<html><body style='font-family:monospace;background:#111;color:#eee;padding:20px;'>"
        "<h1>Message saved.</h1>"
        "<a style='color:#eee;' href='/'>Back</a> "
        "<a style='color:#eee;' href='/messages'>View Messages</a>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t messages_get_handler(httpd_req_t *req) {
    FILE *file = fopen(MESSAGE_FILE, "r");

    httpd_resp_set_type(req, "text/html");

    httpd_resp_sendstr_chunk(req,
        "<html><body style='font-family:monospace;background:#111;color:#eee;padding:20px;'>"
        "<h1>Saved Messages</h1>"
        "<pre style='white-space:pre-wrap;border:1px solid #555;padding:10px;'>"
    );

    if (file == NULL) {
        httpd_resp_sendstr_chunk(req, "No messages saved yet.");
    } else {
        char line[256];

        while (fgets(line, sizeof(line), file)) {
            httpd_resp_sendstr_chunk(req, line);
        }

        fclose(file);
    }

    httpd_resp_sendstr_chunk(req,
        "</pre>"
        "<a style='color:#eee;' href='/'>Back</a> "
        "<a style='color:#eee;' href='/wipe'>Wipe Messages</a>"
        "</body></html>"
    );

    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

esp_err_t wipe_get_handler(httpd_req_t *req) {
    wipe_messages();

    const char *response =
        "<html><body style='font-family:monospace;background:#111;color:#eee;padding:20px;'>"
        "<h1>Messages wiped.</h1>"
        "<a style='color:#eee;' href='/'>Back</a>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Web server setup
 * -------------------------------------------------------------------------- */

void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };

        httpd_uri_t submit = {
            .uri = "/submit",
            .method = HTTP_POST,
            .handler = submit_post_handler,
            .user_ctx = NULL
        };

        httpd_uri_t messages = {
            .uri = "/messages",
            .method = HTTP_GET,
            .handler = messages_get_handler,
            .user_ctx = NULL
        };

        httpd_uri_t wipe = {
            .uri = "/wipe",
            .method = HTTP_GET,
            .handler = wipe_get_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &submit);
        httpd_register_uri_handler(server, &messages);
        httpd_register_uri_handler(server, &wipe);

        ESP_LOGI(TAG, "Web server started");
    }
}

/* --------------------------------------------------------------------------
 * Wi-Fi AP setup
 * -------------------------------------------------------------------------- */

void start_wifi_ap(void) {
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .password = AP_PASS,
            .max_connection = MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Wi-Fi AP started");
    ESP_LOGI(TAG, "SSID: %s", AP_SSID);
    ESP_LOGI(TAG, "Password: %s", AP_PASS);
    ESP_LOGI(TAG, "Open browser to: http://192.168.4.1");
}

/* --------------------------------------------------------------------------
 * UART support for Flipper commands
 * -------------------------------------------------------------------------- */

void uart_send_line(const char *text) {
    uart_write_bytes(FLIPPER_UART, text, strlen(text));
    uart_write_bytes(FLIPPER_UART, "\n", 1);
}

void init_flipper_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(
        FLIPPER_UART,
        UART_BUFFER_SIZE,
        UART_BUFFER_SIZE,
        0,
        NULL,
        0
    );

    uart_param_config(FLIPPER_UART, &uart_config);

    /*
    * Use the default UART0 pins already routed
    * by the Flipper Wi-Fi Dev Board PCB.
    */
    uart_set_pin(
        FLIPPER_UART,
        UART_TX_PIN,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );

    uart_send_line("PROTODROP READY");
}

void handle_uart_command(const char *cmd) {
    if (strcmp(cmd, "STATUS") == 0) {
        uart_send_line("STATUS: ProtoDrop running");
    } else if (strcmp(cmd, "COUNT") == 0) {
        char response[64];
        snprintf(response, sizeof(response), "COUNT: %d", count_messages());
        uart_send_line(response);
    } else if (strcmp(cmd, "WIPE") == 0) {
        wipe_messages();
        uart_send_line("WIPE: Messages erased");
    } else if (strcmp(cmd, "HELP") == 0) {
        uart_send_line("CMD: STATUS COUNT LATEST GET WIPE HELP");
    }    else if (strcmp(cmd, "LATEST") == 0) {
        char response[128];
        get_latest_message(response, sizeof(response));
        uart_send_line(response);
        }
       else if(strncmp(cmd, "GET ", 4) == 0) {
    int msg_number = atoi(cmd + 4);

    char response[128];
    get_message_by_number(msg_number, response, sizeof(response));

    uart_send_line(response);
}
    else {
        uart_send_line("ERROR: Unknown command");
    }


}

void uart_command_task(void *arg) {
    uint8_t data[128];

    while (1) {
        int len = uart_read_bytes(
            FLIPPER_UART,
            data,
            sizeof(data) - 1,
            pdMS_TO_TICKS(100)
        );

        if (len > 0) {
            data[len] = '\0';

            char *cmd = (char *)data;

            cmd[strcspn(cmd, "\r\n")] = '\0';

            ESP_LOGI(TAG, "UART command received: %s", cmd);

            handle_uart_command(cmd);
        }
    }
}

/* --------------------------------------------------------------------------
 * Main entry point
 * -------------------------------------------------------------------------- */

void app_main(void) {
    ESP_LOGI(TAG, "Starting ProtoDrop...");

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    init_spiffs();

    start_wifi_ap();
    start_webserver();

    init_flipper_uart();

    xTaskCreate(
        uart_command_task,
        "uart_command_task",
        4096,
        NULL,
        5,
        NULL
    );
}