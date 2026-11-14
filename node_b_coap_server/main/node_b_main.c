/*
 * NODE B - ESP32-H2 CoAP Server
 *
 * Nhan lenh CoAP PUT /relay "ON" hoac "OFF" tu Node A (Border Router)
 * qua mang Thread, dieu khien GPIO12 bat/tat Relay hoac LED.
 *
 * HUONG DAN SU DUNG:
 * -----------------------------------------------------------------
 * 1. Nap firmware nay vao ESP32-H2 (Node B) o COM9
 * 2. Tren Node A (Border Router C6), tao mang Thread:
 *        ot dataset init new
 *        ot dataset commit active
 *        ot ifconfig up
 *        ot thread start
 *    Cho den khi: ot state -> "leader"
 *
 * 3. Tren Node A, lay dataset de chia se:
 *        ot dataset active -x
 *    (copy chuoi hex ra)
 *
 * 4. Tren Node B (H2 nay), nhap dataset va gia nhap mang:
 *        ot dataset set active <dan-chuoi-hex-vua-copy>
 *        ot ifconfig up
 *        ot thread start
 *    Cho den khi: ot state -> "child" hoac "router"
 *
 * 5. Tren Node B, lay dia chi IPv6:
 *        ot ipaddr
 *    (chon dia chi bat dau bang fd... khong phai fe80)
 *
 * 6. Tren Node A, gui lenh bat/tat/doc trang thai relay:
 *        ot coap start
 *        ot coap put <dia-chi-ipv6-cua-B> relay con ON    (Bat relay)
 *        ot coap put <dia-chi-ipv6-cua-B> relay con OFF   (Tat relay)
 *        ot coap get <dia-chi-ipv6-cua-B> relay con       (Doc trang thai ON/OFF)
 * -----------------------------------------------------------------
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "openthread/coap.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/srp_client.h"
#include "openthread/joiner.h"
#include "ot_examples_common.h"

#define TAG         "NODE_B"
#define RELAY_GPIO  GPIO_NUM_12

/*
 * Cau hinh Active-Low cho Relay:
 * ON  -> muc 0 (keo xuong GND de kich relay)
 * OFF -> muc 1 (keo len VCC de ngat relay)
 */
#define RELAY_LEVEL_ON   0
#define RELAY_LEVEL_OFF  1

/* ========================================================================= */
/*  GPIO12 - Relay / LED & State                                             */
/* ========================================================================= */

static bool s_relay_state = false;

static void relay_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(RELAY_GPIO, RELAY_LEVEL_OFF);
    s_relay_state = false;
    ESP_LOGI(TAG, "GPIO%d configured as OUTPUT (Active-Low: ON=0, OFF=1) - initial OFF (level 1)", RELAY_GPIO);
}

/* ========================================================================= */
/*  CoAP Server - resource /relay                                            */
/* ========================================================================= */

static otCoapResource s_relay_resource;

/*
 * Handler duoc goi khi co request CoAP den /relay.
 * - GET: doc trang thai hien tai (tra ve 2.05 Content voi payload "ON" hoac "OFF")
 * - PUT / POST: dieu khien bat/tat relay ("ON", "OFF", "1", "0") -> tra ve 2.04 Changed
 */
static void relay_request_handler(void *aContext, otMessage *aMessage,
                                  const otMessageInfo *aMessageInfo)
{
    otInstance *instance = esp_openthread_get_instance();
    otCoapCode  code     = otCoapMessageGetCode(aMessage);
    otCoapType  type     = otCoapMessageGetType(aMessage);

    /* --- 1. Xu ly lenh GET: Doc trang thai relay (khong thay doi GPIO) --- */
    if (code == OT_COAP_CODE_GET) {
        ESP_LOGI(TAG, "Received GET /relay -> current state: %s",
                 s_relay_state ? "ON" : "OFF");

        otMessage *response = otCoapNewMessage(instance, NULL);
        if (response != NULL) {
            otCoapType resp_type = (type == OT_COAP_TYPE_CONFIRMABLE)
                                       ? OT_COAP_TYPE_ACKNOWLEDGMENT
                                       : OT_COAP_TYPE_NON_CONFIRMABLE;

            otError err = otCoapMessageInitResponse(response, aMessage,
                                                    resp_type,
                                                    OT_COAP_CODE_CONTENT);
            if (err == OT_ERROR_NONE) {
                otCoapMessageSetPayloadMarker(response);
                const char *resp_text = s_relay_state ? "ON" : "OFF";
                otMessageAppend(response, resp_text, (uint16_t)strlen(resp_text));
                otCoapSendResponse(instance, response, aMessageInfo);
                ESP_LOGI(TAG, "Sent CoAP 2.05 CONTENT: \"%s\"", resp_text);
            } else {
                otMessageFree(response);
            }
        }
        return;
    }

    /* --- 2. Xu ly lenh PUT hoac POST: Dieu khien relay --- */
    if (code == OT_COAP_CODE_PUT || code == OT_COAP_CODE_POST) {
        /* Doc payload */
        char     payload[16] = {0};
        uint16_t offset = otMessageGetOffset(aMessage);
        uint16_t length = otMessageGetLength(aMessage) - offset;
        if (length > sizeof(payload) - 1) {
            length = sizeof(payload) - 1;
        }
        otMessageRead(aMessage, offset, payload, length);
        payload[length] = '\0';

        bool new_state = false;
        bool valid     = true;

        if (strcasecmp(payload, "ON") == 0 || strcmp(payload, "1") == 0) {
            new_state = true;
        } else if (strcasecmp(payload, "OFF") == 0 || strcmp(payload, "0") == 0) {
            new_state = false;
        } else {
            ESP_LOGW(TAG, "Unknown payload: \"%s\" (expected ON/OFF/1/0)", payload);
            valid = false;
        }

        if (valid) {
            s_relay_state = new_state;
            uint32_t level = s_relay_state ? RELAY_LEVEL_ON : RELAY_LEVEL_OFF;
            gpio_set_level(RELAY_GPIO, level);
            ESP_LOGI(TAG, "============================================");
            ESP_LOGI(TAG, "  RELAY %s  (GPIO%d = %lu)",
                     s_relay_state ? "ON " : "OFF", RELAY_GPIO, (unsigned long)level);
            ESP_LOGI(TAG, "============================================");
        }

        /* Gui CoAP response (ACK) neu client yeu cau Confirmable */
        if (type == OT_COAP_TYPE_CONFIRMABLE) {
            otMessage *response = otCoapNewMessage(instance, NULL);
            if (response != NULL) {
                otCoapCode resp_code = valid ? OT_COAP_CODE_CHANGED
                                             : OT_COAP_CODE_BAD_REQUEST;
                otError err = otCoapMessageInitResponse(response, aMessage,
                                                        OT_COAP_TYPE_ACKNOWLEDGMENT,
                                                        resp_code);
                if (err == OT_ERROR_NONE) {
                    otCoapMessageSetPayloadMarker(response);
                    const char *resp_text = valid ? (s_relay_state ? "ON" : "OFF") : "ERROR";
                    otMessageAppend(response, resp_text, (uint16_t)strlen(resp_text));
                    otCoapSendResponse(instance, response, aMessageInfo);
                    ESP_LOGI(TAG, "Sent CoAP ACK (2.04 CHANGED) with payload: \"%s\"", resp_text);
                } else {
                    otMessageFree(response);
                }
            }
        }
        return;
    }

    ESP_LOGW(TAG, "Received unsupported CoAP code %d on /relay, ignoring", code);
}

static void coap_server_init(void)
{
    otInstance *instance = esp_openthread_get_instance();

    memset(&s_relay_resource, 0, sizeof(s_relay_resource));
    s_relay_resource.mUriPath = "relay";
    s_relay_resource.mHandler = relay_request_handler;
    s_relay_resource.mContext = NULL;
    s_relay_resource.mNext   = NULL;

    otCoapAddResource(instance, &s_relay_resource);
    otCoapStart(instance, OT_DEFAULT_COAP_PORT);

    ESP_LOGI(TAG, "CoAP server started - port %d, resource: /relay", OT_DEFAULT_COAP_PORT);
}

/* ========================================================================= */
/*  SRP Client Registration                                                  */
/* ========================================================================= */

#define NODE_SRP_NAME "relay-01"
#define NODE_SRP_TYPE "_relay._udp"

static otIp6Address s_node_srp_ipv6;
static otSrpClientService s_node_srp_service = {
    .mName         = NODE_SRP_TYPE,
    .mInstanceName = NODE_SRP_NAME,
    .mPort         = 5683,
};

/* Hàm này chỉ được gọi khi đã giữ OpenThread lock. */
static otError node_srp_register_locked(otInstance *instance)
{
    otError err = otSrpClientSetHostName(instance, NODE_SRP_NAME);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SRP SetHostName failed: %d", (int)err);
        return err;
    }
    s_node_srp_ipv6 = *otThreadGetMeshLocalEid(instance);
    err = otSrpClientSetHostAddresses(instance, &s_node_srp_ipv6, 1);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SRP SetHostAddresses failed: %d", (int)err);
        return err;
    }
    err = otSrpClientAddService(instance, &s_node_srp_service);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SRP AddService failed: %d", (int)err);
        return err;
    }
    otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    return OT_ERROR_NONE;
}

static void node_srp_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool attempted = false;
        otError err = OT_ERROR_NONE;
        if (esp_openthread_lock_acquire(portMAX_DELAY)) {
            otInstance *instance = esp_openthread_get_instance();
            if (instance != NULL) {
                otDeviceRole role = otThreadGetDeviceRole(instance);
                if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
                    err = node_srp_register_locked(instance);
                    attempted = true;
                }
            }
            esp_openthread_lock_release();
        }
        if (attempted) {
            if (err == OT_ERROR_NONE) {
                ESP_LOGI(TAG, "SRP registration initiated: %s (%s)", NODE_SRP_NAME, NODE_SRP_TYPE);
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelete(NULL);
}

/* ========================================================================= */
/*  Pair Button (BOOT GPIO9) Long Press Handling & Joiner                    */
/* ========================================================================= */

#define PAIR_BUTTON_GPIO GPIO_NUM_9
#define PAIR_HOLD_US     INT64_C(2000000)

static QueueHandle_t s_join_result_queue = NULL;

static void pair_join_callback(otError result, void *context)
{
    (void)context;
    if (s_join_result_queue != NULL) {
        xQueueOverwrite(s_join_result_queue, &result);
    }
}

static void pair_try_join(void)
{
    if (!esp_openthread_lock_acquire(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Cannot acquire OpenThread lock for pairing");
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        esp_openthread_lock_release();
        return;
    }

    if (otDatasetIsCommissioned(instance)) {
        esp_openthread_lock_release();
        ESP_LOGI(TAG, "PAIR_SKIPPED_ALREADY_COMMISSIONED");
        return;
    }

    /* Bat IPv6 neu chua bat */
    if (!otIp6IsEnabled(instance)) {
        otError ip_err = otIp6SetEnabled(instance, true);
        if (ip_err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "PAIR_FAILED: otIp6SetEnabled error=%d", (int)ip_err);
            esp_openthread_lock_release();
            return;
        }
    }

    /* Xoa event cu trong queue truoc khi bat dau lan ghep moi */
    if (s_join_result_queue != NULL) {
        xQueueReset(s_join_result_queue);
    }

    otError err = otJoinerStart(instance, "R8K4M9", NULL, NULL, NULL, NULL, NULL, pair_join_callback, NULL);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "PAIR_START_FAILED: error=%d", (int)err);
        return;
    }

    ESP_LOGI(TAG, "PAIR_STARTED: Joiner running (waiting up to 60s)...");

    /* Cho ket qua tu callback toi da 60 giay (ngoai OpenThread lock) */
    otError join_result = OT_ERROR_NONE;
    BaseType_t recv = xQueueReceive(s_join_result_queue, &join_result, pdMS_TO_TICKS(60000));

    if (recv == pdTRUE) {
        if (join_result == OT_ERROR_NONE) {
            if (esp_openthread_lock_acquire(portMAX_DELAY)) {
                otError th_err = otThreadSetEnabled(instance, true);
                esp_openthread_lock_release();
                if (th_err == OT_ERROR_NONE) {
                    ESP_LOGI(TAG, "PAIR_SUCCESS: Commissioning done, Thread enabled!");
                } else {
                    ESP_LOGE(TAG, "PAIR_SUCCESS but otThreadSetEnabled failed: %d", (int)th_err);
                }
            }
        } else {
            ESP_LOGE(TAG, "PAIR_FAILED: error=%d", (int)join_result);
        }
        return;
    }

    /* Neu het 60s, lay lai lock va kiem tra queue lan cuoi voi timeout 0 */
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        if (xQueueReceive(s_join_result_queue, &join_result, 0) == pdTRUE) {
            if (join_result == OT_ERROR_NONE) {
                otError th_err = otThreadSetEnabled(instance, true);
                esp_openthread_lock_release();
                if (th_err == OT_ERROR_NONE) {
                    ESP_LOGI(TAG, "PAIR_SUCCESS: Commissioning done, Thread enabled!");
                } else {
                    ESP_LOGE(TAG, "PAIR_SUCCESS but otThreadSetEnabled failed: %d", (int)th_err);
                }
            } else {
                esp_openthread_lock_release();
                ESP_LOGE(TAG, "PAIR_FAILED: error=%d", (int)join_result);
            }
        } else {
            otJoinerStop(instance);
            esp_openthread_lock_release();
            ESP_LOGW(TAG, "PAIR_TIMEOUT");
        }
    }
}

static void pair_button_task(void *arg)
{
    (void)arg;
    int64_t pressed_since = -1;
    unsigned released_samples = 0;
    bool fired = false;

    for (;;) {
        if (gpio_get_level(PAIR_BUTTON_GPIO) == 0) {
            released_samples = 0;
            int64_t now = esp_timer_get_time();
            if (pressed_since < 0) {
                pressed_since = now;
            }
            if (!fired && now - pressed_since >= PAIR_HOLD_US) {
                fired = true;
                ESP_LOGI(TAG, "PAIR_BUTTON_LONG_PRESS");
                pair_try_join();
            }
        } else {
            pressed_since = -1;
            // Chỉ cho phép lần nhấn mới sau khi nút đã thả ổn định.
            if (released_samples < 3 && ++released_samples == 3) {
                fired = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void pair_button_start(void)
{
    s_join_result_queue = xQueueCreate(1, sizeof(otError));
    if (s_join_result_queue == NULL) {
        ESP_LOGE(TAG, "Cannot create join result queue");
        return;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    if (xTaskCreate(pair_button_task, "pair_button", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create pair button task");
        vQueueDelete(s_join_result_queue);
        s_join_result_queue = NULL;
    }
}

/* ========================================================================= */
/*  app_main                                                                 */
/* ========================================================================= */

void app_main(void)
{
    /* --- 1. Khoi tao he thong --- */
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3, /* netif + task queue + radio */
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    /* --- 2. Khoi tao GPIO12 cho Relay/LED --- */
    relay_gpio_init();

    /* --- 3. Bat CLI de cau hinh mang Thread --- */
#if CONFIG_OPENTHREAD_CLI
    ot_console_start();
    ot_register_external_commands();
#endif

    /* --- 4. Khoi dong OpenThread (native radio tren H2) --- */
    static esp_openthread_config_t config = {
#if CONFIG_OPENTHREAD_PLATFORM_NETIF
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
#endif
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };

    ESP_ERROR_CHECK(esp_openthread_start(&config));

    /* --- 5. Dang ky CoAP server & Tu dong ket noi lai mang Thread (Auto-Reconnect) --- */
    esp_openthread_lock_acquire(portMAX_DELAY);
    coap_server_init();

    otInstance *instance = esp_openthread_get_instance();
    if (otDatasetIsCommissioned(instance)) {
        ESP_LOGI(TAG, "Saved Thread network found in NVS - auto reconnecting...");
        otError err = otIp6SetEnabled(instance, true);
        if (err == OT_ERROR_NONE) {
            err = otThreadSetEnabled(instance, true);
        }
        if (err == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Thread auto-start OK!");
        } else {
            ESP_LOGE(TAG, "Thread auto-start failed (error %d)", err);
        }
    } else {
        ESP_LOGW(TAG, "No Thread dataset in NVS - waiting for commissioning via CLI");
    }
    esp_openthread_lock_release();

    if (xTaskCreate(node_srp_task, "node_srp", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create SRP task");
    }

    /* --- 6. Task xu ly nut nhan BOOT (Pair Button) --- */
    pair_button_start();

    /* --- 7. In huong dan --- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  NODE B (CoAP Relay Server) READY!");
    ESP_LOGI(TAG, "  Relay/LED pin: GPIO%d (Active-Low)", RELAY_GPIO);
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Neu chua co dataset, gia nhap mang bang lenh CLI:");
    ESP_LOGI(TAG, "  ot dataset set active <hex-tu-Border-Router>");
    ESP_LOGI(TAG, "  ot ifconfig up");
    ESP_LOGI(TAG, "  ot thread start");
    ESP_LOGI(TAG, "");
}
