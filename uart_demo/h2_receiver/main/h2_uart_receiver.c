/*
 * ESP32-H2 - GIAO TIEP 2 CHIEU (CHAT 2 WAY)
 * 
 * - Go noi dung o monitor cua H2 -> gui sang C6 qua UART1 (TX=GPIO11, RX=GPIO10)
 * - Khi C6 gui sang -> H2 in ngay tin nhan len man hinh va tra ve ACK
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#define LINK_UART        UART_NUM_1
#define LINK_TX_PIN      11
#define LINK_RX_PIN      10
#define LINK_BAUD        115200
#define LINK_BUF_SIZE    256

#define CONSOLE_UART     UART_NUM_0
#define MAX_INPUT_LEN    100

static const char *TAG = "H2";
static uint32_t g_tx_seq = 0;

/* Tinh CRC-8 */
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void uart_init_all(void)
{
    /* 1. UART1 ket noi voi C6 */
    const uart_config_t link_cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, LINK_BUF_SIZE * 2, LINK_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &link_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* 2. UART0 Console (Ban phim & man hinh monitor cua H2) */
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
}

/* In chuoi ra console */
static void console_print(const char *str)
{
    uart_write_bytes(CONSOLE_UART, str, strlen(str));
}

/* ========================================================================= */
/* TASK 1: Lang nghe tin nhan & ACK tu C6                                   */
/* ========================================================================= */
static void rx_task(void *pvParameters)
{
    char line[LINK_BUF_SIZE];
    size_t n = 0;

    while (true) {
        uint8_t c;
        if (uart_read_bytes(LINK_UART, &c, 1, portMAX_DELAY) <= 0) {
            continue;
        }

        if (c == '\r') continue;
        if (c == '\n') {
            line[n] = '\0';
            if (n > 0) {
                /* Xu ly goi tin */
                if (strncmp(line, "MSG,", 4) == 0) {
                    char *body = line + 4;
                    char *last_comma = strrchr(body, ',');
                    if (last_comma != NULL && strlen(last_comma + 1) == 2) {
                        *last_comma = '\0';
                        unsigned long rx_crc = strtoul(last_comma + 1, NULL, 16);
                        unsigned long seq = strtoul(body, NULL, 10);
                        char *payload = strchr(body, ',') ? (strchr(body, ',') + 1) : "";
                        uint8_t calc_crc = crc8((const uint8_t *)body, strlen(body));

                        if (calc_crc == rx_crc) {
                            /* In tin nhan nhan duoc ra man hinh */
                            char msg_out[256];
                            snprintf(msg_out, sizeof(msg_out), "\r\n\033[1;32m[C6 gui den]:\033[0m %s\r\nNhap tin nhan: ", payload);
                            console_print(msg_out);

                            /* Tra loi ACK */
                            char ack[32];
                            snprintf(ack, sizeof(ack), "ACK,%lu\n", seq);
                            uart_write_bytes(LINK_UART, ack, strlen(ack));
                        } else {
                            char nak[32];
                            snprintf(nak, sizeof(nak), "NAK,%lu\n", seq);
                            uart_write_bytes(LINK_UART, nak, strlen(nak));
                        }
                    }
                } else if (strncmp(line, "ACK,", 4) == 0) {
                    unsigned long ack_seq = strtoul(line + 4, NULL, 10);
                    char ack_out[128];
                    snprintf(ack_out, sizeof(ack_out), "\r\n\033[1;34m[Thong bao]:\033[0m C6 da nhan duoc tin #%lu\r\nNhap tin nhan: ", ack_seq);
                    console_print(ack_out);
                }
            }
            n = 0;
            continue;
        }

        if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

/* ========================================================================= */
/* TASK 2: Doc ban phim va gui sang C6                                      */
/* ========================================================================= */
static void console_input_task(void *pvParameters)
{
    char input_buf[MAX_INPUT_LEN + 1];
    size_t n = 0;

    vTaskDelay(pdMS_TO_TICKS(500));
    console_print("\r\n========================================");
    console_print("\r\n CHAT BOX ESP32-H2 -> C6 SAN SANG!");
    console_print("\r\n========================================\r\n");
    console_print("Nhap tin nhan: ");

    while (true) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) <= 0) {
            continue;
        }

        /* Phim Enter */
        if (c == '\r' || c == '\n') {
            if (n == 0) continue;
            console_print("\r\n");
            input_buf[n] = '\0';

            /* Dong goi: MSG,<seq>,<noi dung>,<CRC> */
            g_tx_seq++;
            char body[160];
            char frame[192];
            int body_len = snprintf(body, sizeof(body), "%lu,%s", (unsigned long)g_tx_seq, input_buf);
            uint8_t crc = crc8((const uint8_t *)body, (size_t)body_len);
            int frame_len = snprintf(frame, sizeof(frame), "MSG,%s,%02X\n", body, crc);

            /* Gui qua UART1 */
            uart_write_bytes(LINK_UART, frame, frame_len);

            console_print("Nhap tin nhan: ");
            n = 0;
            continue;
        }

        /* Phim Backspace */
        if (c == 0x08 || c == 0x7F) {
            if (n > 0) {
                n--;
                console_print("\b \b");
            }
            continue;
        }

        /* Ky tu binh thuong */
        if (n < MAX_INPUT_LEN) {
            input_buf[n++] = (char)c;
            uart_write_bytes(CONSOLE_UART, (const char *)&c, 1); // Echo len man hinh
        }
    }
}

void app_main(void)
{
    uart_init_all();
    xTaskCreate(rx_task, "rx_task", 4096, NULL, 10, NULL);
    xTaskCreate(console_input_task, "console_task", 4096, NULL, 5, NULL);
}
