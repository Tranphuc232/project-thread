/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenThread Border Router Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_coexist.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_openthread_types.h"
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_config.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_eventfd.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "ot_examples_br.h"
#include "ot_examples_common.h"
#include "openthread/coap.h"
#include "openthread/dataset.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#include "openthread/srp_server.h"
#include "sensor_client.h"
#include "actuator_client.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
#include "ext_coex_cmd.h"
#endif

#define TAG "esp_ot_br"

#if CONFIG_OPENTHREAD_SUPPORT_HW_RESET_RCP
#define PIN_TO_RCP_RESET CONFIG_OPENTHREAD_HW_RESET_RCP_PIN
static void rcp_failure_hardware_reset_handler(void)
{
    gpio_config_t reset_pin_config;
    memset(&reset_pin_config, 0, sizeof(reset_pin_config));
    reset_pin_config.intr_type = GPIO_INTR_DISABLE;
    reset_pin_config.pin_bit_mask = BIT(PIN_TO_RCP_RESET);
    reset_pin_config.mode = GPIO_MODE_OUTPUT;
    reset_pin_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    reset_pin_config.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&reset_pin_config);
    gpio_set_level(PIN_TO_RCP_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TO_RCP_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_reset_pin(PIN_TO_RCP_RESET);
}
#endif

static esp_err_t gateway_coap_start(void)
{
    if (!esp_openthread_lock_acquire(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }
    otError err = otCoapStart(instance, 5683);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Gateway CoAP start failed: %d", (int)err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Gateway CoAP ready on port 5683");
    return ESP_OK;
}

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * task queue
    // * border router
    size_t max_eventfd = 3;

#if CONFIG_OPENTHREAD_RADIO_NATIVE || CONFIG_OPENTHREAD_RADIO_SPINEL_SPI
    // * radio driver (A native radio device needs a eventfd for radio driver.)
    // * SpiSpinelInterface (The Spi Spinel Interface needs a eventfd.)
    // The above will not exist at the same time.
    max_eventfd++;
#endif
#if CONFIG_OPENTHREAD_RADIO_TREL
    // * TREL reception (The Thread Radio Encapsulation Link needs a eventfd for reception.)
    max_eventfd++;
#endif
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = max_eventfd,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));
#if CONFIG_OPENTHREAD_SUPPORT_HW_RESET_RCP
    esp_openthread_register_rcp_failure_handler(rcp_failure_hardware_reset_handler);
    esp_openthread_set_coprocessor_reset_failure_callback(rcp_failure_hardware_reset_handler);
#endif

#if CONFIG_OPENTHREAD_CLI
    ot_console_start();
    ot_register_external_commands();
#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
    register_cmd_extcoex();
#endif
#endif

#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
    ot_external_coexist_init();
#endif

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };
ESP_LOGI(TAG, "WROOM_HOST_CHECK: mode=%d UART=%d RX=%d TX=%d baud=%d flow=%d",
         (int)config.platform_config.radio_config.radio_mode,
         (int)config.platform_config.radio_config.radio_uart_config.port,
         (int)config.platform_config.radio_config.radio_uart_config.rx_pin,
         (int)config.platform_config.radio_config.radio_uart_config.tx_pin,
         (int)config.platform_config.radio_config.radio_uart_config.uart_config.baud_rate,
         (int)config.platform_config.radio_config.radio_uart_config.uart_config.flow_ctrl);
    ESP_ERROR_CHECK(esp_openthread_start(&config));
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif
#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif
#if CONFIG_OPENTHREAD_BORDER_ROUTER && CONFIG_OPENTHREAD_NETWORK_AUTO_START
    ESP_ERROR_CHECK(esp_openthread_border_router_start());
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
#endif
#endif

    /* Khoi phuc mang Thread tu NVS neu co Dataset da luu */
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL) {
            otSrpServerSetEnabled(instance, true);
            ESP_LOGI(TAG, "SRP Server enabled on port %u", (unsigned)otSrpServerGetPort(instance));

            if (otDatasetIsCommissioned(instance)) {
                otDeviceRole role = otThreadGetDeviceRole(instance);
                if (role == OT_DEVICE_ROLE_DISABLED) {
                    otError err = otIp6SetEnabled(instance, true);
                    if (err == OT_ERROR_NONE) {
                        err = otThreadSetEnabled(instance, true);
                    }
                    if (err == OT_ERROR_NONE) {
                        ESP_LOGI(TAG, "Saved Thread network restored from NVS (IPv6 & Thread enabled)");
                    } else {
                        ESP_LOGE(TAG, "Failed to restore Thread network from NVS: error=%d", (int)err);
                    }
                } else {
                    ESP_LOGI(TAG, "Thread network already running (role=%d)", (int)role);
                }
            } else {
                ESP_LOGW(TAG, "No saved Thread dataset in NVS - waiting for network commissioning via CLI");
            }
        }
        esp_openthread_lock_release();
    }

    ESP_ERROR_CHECK(gateway_coap_start());
    ESP_ERROR_CHECK(sensor_client_start());
    ESP_ERROR_CHECK(actuator_client_start());
}
