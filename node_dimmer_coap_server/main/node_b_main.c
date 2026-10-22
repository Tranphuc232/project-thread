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
#include <string.h>
#include <strings.h>

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

static void dimmer_set_brightness(int brightness)
{
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    /* Cong thuc: duty = brightness * 1023 / 100 */
    uint32_t duty = (uint32_t)(brightness * LEDC_MAX_DUTY / 100);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

    s_current_brightness = brightness;
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  LIGHT level = %d%% | PWM duty = %lu",
             s_current_brightness, (unsigned long)duty);
    ESP_LOGI(TAG, "============================================");
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
                otCoapMessageSetPayloadMarker(response);
                char resp_text[16];
                snprintf(resp_text, sizeof(resp_text), "%d", s_current_brightness);
                otMessageAppend(response, resp_text, (uint16_t)strlen(resp_text));
                otCoapSendResponse(instance, response, aMessageInfo);
                ESP_LOGI(TAG, "Sent CoAP 2.05 CONTENT: \"%s\"", resp_text);
            } else {
                otMessageFree(response);
            }
        }
        return;
    }

    /* --- 2. Xu ly lenh PUT hoac POST: Chinh do sang --- */
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

        /* Parse integer 0..100 */
        char *endptr = NULL;
        long val = strtol(payload, &endptr, 10);
        bool valid = true;

        /* Kiem tra tinh hop le cua chuoi so va khoang gia tri 0..100 */
        if (endptr == payload || (*endptr != '\0' && *endptr != '\r' && *endptr != '\n' && *endptr != ' ')) {
            valid = false;
        } else if (val < 0 || val > 100) {
            valid = false;
        }

        if (valid) {
            dimmer_set_brightness((int)val);
        } else {
            ESP_LOGW(TAG, "Invalid payload: \"%s\" (expected integer 0..100)", payload);
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
                    char resp_text[16];
                    if (valid) {
                        snprintf(resp_text, sizeof(resp_text), "%d", s_current_brightness);
                    } else {
                        snprintf(resp_text, sizeof(resp_text), "ERROR");
                    }
                    otMessageAppend(response, resp_text, (uint16_t)strlen(resp_text));
                    otCoapSendResponse(instance, response, aMessageInfo);
                    ESP_LOGI(TAG, "Sent CoAP ACK payload: \"%s\"", resp_text);
                } else {
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

    /* --- 6. In banner khoi dong --- */
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
