/*
 * NODE DIMMER - ESP32-C6 CoAP Server (PWM Dimmer)
 *
 * Dieu khien do sang den qua PWM tren GPIO22 (0 - 100%)
 * Nhan lenh CoAP PUT /light "<0-100>" hoac GET /light tu Node A (Border Router)
 *
 * HUONG DAN SU DUNG:
 * -----------------------------------------------------------------
 * 1. Nap firmware nay vao ESP32-C6:
 *        cd c:\Users\ASUS\project-name\node_dimmer_coap_server
 *        idf.py set-target esp32c6
 *        idf.py build flash -p <COM_PORT> monitor
 *
 * 2. Gia nhap mang Thread (neu chua co san trong NVS):
 *        ot dataset set active <chuoi-hex-dataset-tu-Node-A>
 *        ot ifconfig up
 *        ot thread start
 *    (Khi da luu vao NVS, cac lan khoi dong sau chip se tu dong attach lai mang)
 *
 * 3. Lay dia chi IPv6 tren Node Dimmer:
 *        ot ipaddr
 *    (chon dia chi fd... Mesh-Local)
 *
 * 4. Tren Node A (Border Router), dieu khien do sang:
 *        ot coap start
 *        ot coap put <dia-chi-ipv6-cua-Dimmer> light con 70    (Chinh do sang 70%)
 *        ot coap put <dia-chi-ipv6-cua-Dimmer> light con 0     (Tat den 0%)
 *        ot coap put <dia-chi-ipv6-cua-Dimmer> light con 100   (Bat toi da 100%)
 *        ot coap get <dia-chi-ipv6-cua-Dimmer> light con       (Doc do sang hien tai)
 * -----------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

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
#include "driver/ledc.h"
#include "openthread/coap.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/srp_client.h"
#include "openthread/joiner.h"
#include "ot_examples_common.h"

#define TAG             "NODE_DIMMER"
#define DIMMER_GPIO     GPIO_NUM_22

/* ========================================================================= */
/*  Cau hinh LEDC PWM (GPIO22, 5 kHz, 10-bit resolution)                     */
/* ========================================================================= */

#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_DUTY_RES   LEDC_TIMER_10_BIT  /* 10-bit: gia tri duty tu 0 den 1023 */
#define LEDC_FREQUENCY  (5000)             /* Tan so PWM 5 kHz */
#define LEDC_MAX_DUTY   (1023)             /* 2^10 - 1 = 1023 */

static int s_current_brightness = 0; /* Do sang hien tai: 0 -> 100% */

static void dimmer_pwm_init(void)
{
    /* 1. Cau hinh Timer cho LEDC */
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* 2. Cau hinh Channel cho GPIO22 */
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = DIMMER_GPIO,
        .duty           = 0, /* Startup an toan: khoi dong mac dinh 0% do sang */
        .hpoint         = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    s_current_brightness = 0;
    ESP_LOGI(TAG, "PWM Dimmer initialized on GPIO%d (freq: %d Hz, res: 10-bit) - initial brightness 0%%",
             DIMMER_GPIO, LEDC_FREQUENCY);
}

static esp_err_t dimmer_set_brightness(int brightness)
{
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    /* Cong thuc: duty = brightness * 1023 / 100 */
    uint32_t duty = (uint32_t)(brightness * LEDC_MAX_DUTY / 100);
    esp_err_t err = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_set_duty failed: %s", esp_err_to_name(err));
        return err;
    }
    err = ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_update_duty failed: %s", esp_err_to_name(err));
        return err;
    }

    s_current_brightness = brightness;
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  LIGHT level = %d%% | PWM duty = %lu",
             s_current_brightness, (unsigned long)duty);
    ESP_LOGI(TAG, "============================================");
    return ESP_OK;
}

/* ========================================================================= */
/*  CoAP Server - resource /light                                            */
/* ========================================================================= */

static otCoapResource s_light_resource;

/*
 * Handler xu ly request CoAP den /light:
 * - GET: doc do sang hien tai (0..100) -> tra ve CoAP 2.05 CONTENT
 * - PUT / POST: nhan payload so nguyen 0..100 -> cap nhat PWM -> tra ve 2.04 CHANGED
 */
static void light_request_handler(void *aContext, otMessage *aMessage,
                                  const otMessageInfo *aMessageInfo)
{
    otInstance *instance = esp_openthread_get_instance();
    otCoapCode  code     = otCoapMessageGetCode(aMessage);
    otCoapType  type     = otCoapMessageGetType(aMessage);

    /* --- 1. Xu ly lenh GET: Doc do sang hien tai (khong thay doi PWM) --- */
    if (code == OT_COAP_CODE_GET) {
        ESP_LOGI(TAG, "Received GET /light -> current level: %d%%", s_current_brightness);

        otMessage *response = otCoapNewMessage(instance, NULL);
        if (response != NULL) {
            otCoapType resp_type = (type == OT_COAP_TYPE_CONFIRMABLE)
                                       ? OT_COAP_TYPE_ACKNOWLEDGMENT
                                       : OT_COAP_TYPE_NON_CONFIRMABLE;

            otError err = otCoapMessageInitResponse(response, aMessage,
                                                    resp_type,
                                                    OT_COAP_CODE_CONTENT);
            if (err == OT_ERROR_NONE) {
                err = otCoapMessageSetPayloadMarker(response);
            }
            char resp_text[16];
            snprintf(resp_text, sizeof(resp_text), "%d", s_current_brightness);
            if (err == OT_ERROR_NONE) {
                err = otMessageAppend(response, resp_text, (uint16_t)strlen(resp_text));
            }
            if (err == OT_ERROR_NONE) {
                err = otCoapSendResponse(instance, response, aMessageInfo);
            }

            if (err == OT_ERROR_NONE) {
                ESP_LOGI(TAG, "Sent CoAP 2.05 CONTENT: \"%s\"", resp_text);
            } else {
                ESP_LOGE(TAG, "Failed to send 2.05 response: %d", err);
                otMessageFree(response);
            }
        }
        return;
    }

    /* --- 2. Xu ly lenh PUT hoac POST: Chinh do sang --- */
    if (code == OT_COAP_CODE_PUT || code == OT_COAP_CODE_POST) {
        /* Doc va kiem tra do dai payload */
        char     payload[16] = {0};
        uint16_t offset = otMessageGetOffset(aMessage);
        uint16_t total  = otMessageGetLength(aMessage);
        bool     valid  = true;
        int      new_brightness = 0;

        if (offset > total) {
            valid = false;
        } else {
            uint16_t length = total - offset;
            if (length == 0 || length >= sizeof(payload)) {
                ESP_LOGW(TAG, "Invalid payload length: %u bytes", (unsigned)length);
                valid = false;
            } else {
                uint16_t bytes_read = otMessageRead(aMessage, offset, payload, length);
                if (bytes_read != length) {
                    ESP_LOGW(TAG, "Payload read incomplete: %u/%u bytes", (unsigned)bytes_read, (unsigned)length);
                    valid = false;
                } else {
                    payload[length] = '\0';
                }
            }
        }

        /* Parse so nguyen 0..100, kiem tra ERANGE va khong cho phep ky tu thua sau khoang trang */
        if (valid) {
            char *endptr = NULL;
            errno = 0;
            long val = strtol(payload, &endptr, 10);
            if (errno == ERANGE || endptr == payload) {
                valid = false;
            } else {
                while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
                    endptr++;
                }
                if (*endptr != '\0' || val < 0 || val > 100) {
                    valid = false;
                } else {
                    new_brightness = (int)val;
                }
            }
        }

        otCoapCode resp_code = OT_COAP_CODE_CHANGED;
        if (!valid) {
            resp_code = OT_COAP_CODE_BAD_REQUEST;
            ESP_LOGW(TAG, "Invalid payload: \"%s\" (expected integer 0..100)", payload);
        } else {
            esp_err_t set_err = dimmer_set_brightness(new_brightness);
            if (set_err != ESP_OK) {
                resp_code = OT_COAP_CODE_INTERNAL_ERROR; /* 5.00 */
                ESP_LOGE(TAG, "Failed to set dimmer brightness: %s", esp_err_to_name(set_err));
            }
        }

        /* Gui CoAP response (ACK) neu client yeu cau Confirmable */
        if (type == OT_COAP_TYPE_CONFIRMABLE) {
            otMessage *response = otCoapNewMessage(instance, NULL);
            if (response != NULL) {
                otError err = otCoapMessageInitResponse(response, aMessage,
                                                        OT_COAP_TYPE_ACKNOWLEDGMENT,
                                                        resp_code);
                if (err == OT_ERROR_NONE) {
                    err = otCoapMessageSetPayloadMarker(response);
                }
                char resp_text[16];
                if (resp_code == OT_COAP_CODE_CHANGED) {
                    snprintf(resp_text, sizeof(resp_text), "%d", s_current_brightness);
                } else if (resp_code == OT_COAP_CODE_INTERNAL_ERROR) {
                    snprintf(resp_text, sizeof(resp_text), "INTERNAL_ERROR");
                } else {
                    snprintf(resp_text, sizeof(resp_text), "ERROR");
                }
                if (err == OT_ERROR_NONE) {
                    err = otMessageAppend(response, resp_text, (uint16_t)strlen(resp_text));
                }
                if (err == OT_ERROR_NONE) {
                    err = otCoapSendResponse(instance, response, aMessageInfo);
                }

                if (err == OT_ERROR_NONE) {
                    ESP_LOGI(TAG, "Sent CoAP ACK (%d) payload: \"%s\"", resp_code, resp_text);
                } else {
                    ESP_LOGE(TAG, "Failed to send CoAP ACK: %d", err);
                    otMessageFree(response);
                }
            }
        }
        return;
    }

    ESP_LOGW(TAG, "Received unsupported CoAP code %d on /light, ignoring", code);
}

static void coap_server_init(void)
{
    otInstance *instance = esp_openthread_get_instance();

    memset(&s_light_resource, 0, sizeof(s_light_resource));
    s_light_resource.mUriPath = "light";
    s_light_resource.mHandler = light_request_handler;
    s_light_resource.mContext = NULL;
    s_light_resource.mNext    = NULL;

    otCoapAddResource(instance, &s_light_resource);
    otCoapStart(instance, OT_DEFAULT_COAP_PORT);

    ESP_LOGI(TAG, "CoAP server started - port %d, resource: /light", OT_DEFAULT_COAP_PORT);
}

/* ========================================================================= */
/*  SRP Client Registration                                                  */
/* ========================================================================= */

static otIp6Address s_srp_addr;
static otSrpClientService s_dimmer_service = {
    .mName = "_dimmer._udp",
    .mInstanceName = "dimmer-test-01",
    .mPort = 5683,
};

/* Goi khi da giu OpenThread lock va node da attach. */
static otError dimmer_srp_register_locked(otInstance *instance)
{
    otError err = otSrpClientSetHostName(instance, "dimmer-test-01");
    if (err != OT_ERROR_NONE) {
        return err;
    }
    s_srp_addr = *otThreadGetMeshLocalEid(instance);
    err = otSrpClientSetHostAddresses(instance, &s_srp_addr, 1);
    if (err != OT_ERROR_NONE) {
        return err;
    }
    err = otSrpClientAddService(instance, &s_dimmer_service);
    if (err == OT_ERROR_NONE) {
        otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    }
    return err;
}

static void dimmer_srp_task(void *pvParameters)
{
    (void)pvParameters;
    otInstance *instance = NULL;
    while ((instance = esp_openthread_get_instance()) == NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while (1) {
        bool attached = false;
        if (esp_openthread_lock_acquire(portMAX_DELAY)) {
            otDeviceRole role = otThreadGetDeviceRole(instance);
            attached = (role == OT_DEVICE_ROLE_CHILD ||
                        role == OT_DEVICE_ROLE_ROUTER ||
                        role == OT_DEVICE_ROLE_LEADER);
            if (attached) {
                otError err = dimmer_srp_register_locked(instance);
                if (err == OT_ERROR_NONE) {
                    ESP_LOGI(TAG, "SRP client registration initiated (dimmer-test-01, _dimmer._udp:5683)");
                } else {
                    ESP_LOGE(TAG, "dimmer_srp_register_locked failed: error=%d", (int)err);
                }
            }
            esp_openthread_lock_release();
        }

        if (attached) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    otError err = otJoinerStart(instance, "P7K4M9", NULL, NULL, NULL, NULL, NULL, pair_join_callback, NULL);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "PAIR_START_FAILED: error=%d", (int)err);
        return;
    }

    ESP_LOGI(TAG, "PAIR_STARTED: Joiner running, PSKd=P7K4M9 (waiting up to 60s)...");

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
    /* --- 1. Khoi tao he thong co ban --- */
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3, /* netif + task queue + radio */
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    /* --- 2. Khoi tao PWM GPIO22 cho Dimmer (Safe Startup: 0% do sang) --- */
    dimmer_pwm_init();

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

    /* --- 5. Dang ky CoAP server & Tu dong ket noi mang Thread (Auto-Reconnect) --- */
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

    /* --- 6. Task tu dong dang ky SRP khi attach vao mang --- */
    xTaskCreate(dimmer_srp_task, "dimmer_srp", 4096, NULL, 4, NULL);

    /* --- 7. Task xu ly nut nhan BOOT (Pair Button) --- */
    pair_button_start();

    /* --- 8. In banner khoi dong --- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  NODE DIMMER (CoAP PWM Dimmer) READY!");
    ESP_LOGI(TAG, "  PWM pin: GPIO%d (0 - 100%%)", DIMMER_GPIO);
    ESP_LOGI(TAG, "  CoAP Resource: /light");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Neu chua co dataset, gia nhap mang bang lenh CLI:");
    ESP_LOGI(TAG, "  ot dataset set active <hex-tu-Border-Router>");
    ESP_LOGI(TAG, "  ot ifconfig up");
    ESP_LOGI(TAG, "  ot thread start");
    ESP_LOGI(TAG, "");
}
