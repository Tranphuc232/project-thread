/*
 * NODE SENSOR - ESP32-C6 CoAP Server (DHT11 Temperature & Humidity)
 *
 * Doc nhiet do, do am tu DHT11 tren GPIO10 dinh ky 5 giay mot lan.
 * Cung cap CoAP GET /sensor tra ve JSON:
 * {
 *   "node_id": "sensor_01",
 *   "sensor": "DHT11",
 *   "temperature_c": 28.0,
 *   "humidity_pct": 67.0,
 *   "sample_seq": 12,
 *   "age_ms": 1250,
 *   "last_read_ok": true,
 *   "stale": false
 * }
 *
 * HUONG DAN SU DUNG:
 * -----------------------------------------------------------------
 * 1. Dau noi DHT11 voi ESP32-C6 (COM8):
 *        VCC -> 3V3
 *        DAT -> GPIO10
 *        GND -> GND
 *
 * 2. Nap firmware vao ESP32-C6:
 *        cd c:\Users\ASUS\project-name\node_sensor_coap_server
 *        idf.py set-target esp32c6
 *        idf.py build flash -p COM8 monitor
 *
 * 3. Gia nhap mang Thread (neu chua luu san trong NVS):
 *        ot dataset set active <chuoi-hex-tu-Border-Router>
 *        ot ifconfig up
 *        ot thread start
 *    (Cac lan khoi dong sau chip se tu dong attach lai mang nho NVS)
 *
 * 4. Lay dia chi IPv6 tren Node Sensor:
 *        ot ipaddr
 *    (chon dia chi fd... Mesh-Local)
 *
 * 5. Tren Node A (Border Router), lay du lieu cam bien:
 *        ot coap start
 *        ot coap get <dia-chi-ipv6-cua-Sensor> sensor con
 * -----------------------------------------------------------------
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "dht.h"

#include "openthread/coap.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/srp_client.h"
#include "openthread/joiner.h"
#include "ot_examples_common.h"
#include "openthread/link.h"

#define TAG                     "NODE_SENSOR"
#define DHT_GPIO                GPIO_NUM_10
#define SENSOR_NODE_ID          "sensor_01"

#define SENSOR_SAMPLE_PERIOD_MS 5000   /* Chu ky lay mau: 5000 ms (5s) */
#define SENSOR_STARTUP_DELAY_MS 2000   /* Cho on dinh nguon sau khi bat: 2000 ms */
#define SENSOR_STALE_THRESH_MS  15000  /* Nguong coi la cu (stale): 15000 ms (15s) */
#define SENSOR_POLL_PERIOD_MS 1000     /* Chu ky lay du lieu tu parent: 1000ms (1s) */

/* ========================================================================= */
/*  Cau truc du lieu cam bien & Mutex                                        */
/* ========================================================================= */

typedef struct {
    float    temperature_c;    /* Nhiet do hop le gan nhat (do C) */
    float    humidity_pct;     /* Do am hop le gan nhat (%) */
    bool     has_valid_sample; /* Da tung doc thanh cong it nhat 1 lan chua */
    bool     last_read_ok;     /* Lan doc moi nhat co thanh cong khong */
    uint32_t sample_seq;       /* So thu tu mau doc thanh cong */
    int64_t  last_success_ms;  /* Moc thoi gian khi doc thanh cong gan nhat */
    uint32_t read_error_count; /* Tong so lan doc loi */
} sensor_data_t;

static sensor_data_t s_sensor_data = {
    .temperature_c    = 0.0f,
    .humidity_pct     = 0.0f,
    .has_valid_sample = false,
    .last_read_ok     = false,
    .sample_seq       = 0,
    .last_success_ms  = 0,
    .read_error_count = 0,
};

static SemaphoreHandle_t s_sensor_mutex = NULL;

static inline int64_t get_current_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ========================================================================= */
/*  Task lay mau DHT11 dinh ky                                               */
/* ========================================================================= */

static void sensor_sampling_task(void *pvParameters)
{
    /* Cho sau cap nguon truoc lan doc dau tien */
    vTaskDelay(pdMS_TO_TICKS(SENSOR_STARTUP_DELAY_MS));
    ESP_LOGI(TAG, "Sensor task started on GPIO%d (sampling every %d ms)",
             DHT_GPIO, SENSOR_SAMPLE_PERIOD_MS);

    while (1) {
        float humidity = 0.0f;
        float temperature = 0.0f;

        /* Doc DHT11 qua driver esp-idf-lib/dht */
        esp_err_t res = dht_read_float_data(DHT_TYPE_DHT11, DHT_GPIO, &humidity, &temperature);

        /* Kiem tra tinh hop le cua ket qua doc */
        bool read_ok = (res == ESP_OK) &&
                       !isnan(temperature) && !isinf(temperature) &&
                       !isnan(humidity) && !isinf(humidity) &&
                       (humidity >= 0.0f && humidity <= 100.0f);

        int64_t now_ms = get_current_time_ms();

        /* Cap nhat du lieu voi Mutex */
        xSemaphoreTake(s_sensor_mutex, portMAX_DELAY);
        if (read_ok) {
            s_sensor_data.temperature_c    = temperature;
            s_sensor_data.humidity_pct     = humidity;
            s_sensor_data.has_valid_sample = true;
            s_sensor_data.last_read_ok     = true;
            s_sensor_data.sample_seq++;
            s_sensor_data.last_success_ms  = now_ms;

            ESP_LOGI(TAG, "DHT11 read OK: temperature=%.1f C humidity=%.1f %%", temperature, humidity);
            ESP_LOGI(TAG, "sample_seq=%" PRIu32, s_sensor_data.sample_seq);
        } else {
            s_sensor_data.last_read_ok = false;
            s_sensor_data.read_error_count++;

            int64_t age_ms = s_sensor_data.has_valid_sample ? (now_ms - s_sensor_data.last_success_ms) : -1;
            ESP_LOGW(TAG, "DHT11 read failed: %s", esp_err_to_name(res));
            if (s_sensor_data.has_valid_sample) {
                ESP_LOGW(TAG, "keeping previous sample, age_ms=%" PRId64, age_ms);
            } else {
                ESP_LOGW(TAG, "no valid sample available yet");
            }
        }
        xSemaphoreGive(s_sensor_mutex);

        vTaskDelay(pdMS_TO_TICKS(SENSOR_SAMPLE_PERIOD_MS));
    }
}

/* ========================================================================= */
/*  CoAP Server - resource /sensor                                           */
/* ========================================================================= */

static otCoapResource s_sensor_resource;

static void sensor_request_handler(void *aContext, otMessage *aMessage,
                                   const otMessageInfo *aMessageInfo)
{
    otInstance *instance = esp_openthread_get_instance();
    otCoapCode  code     = otCoapMessageGetCode(aMessage);
    otCoapType  type     = otCoapMessageGetType(aMessage);

    /* Kieu response tuong ung voi request: CON -> ACK, NON -> NON */
    otCoapType resp_type = (type == OT_COAP_TYPE_CONFIRMABLE)
                               ? OT_COAP_TYPE_ACKNOWLEDGMENT
                               : OT_COAP_TYPE_NON_CONFIRMABLE;

    /* Chi ho tro GET tren /sensor. PUT/POST se tra ve 4.05 Method Not Allowed */
    if (code != OT_COAP_CODE_GET) {
        ESP_LOGW(TAG, "Received non-GET request (%d) on /sensor -> 4.05 Method Not Allowed", code);

        otMessage *response = otCoapNewMessage(instance, NULL);
        if (response != NULL) {
            otError err = otCoapMessageInitResponse(response, aMessage,
                                                    resp_type,
                                                    OT_COAP_CODE_METHOD_NOT_ALLOWED);
            if (err == OT_ERROR_NONE) {
                err = otCoapSendResponse(instance, response, aMessageInfo);
            }
            if (err != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "Failed to send 4.05 response: %d", err);
                otMessageFree(response);
            }
        }
        return;
    }

    ESP_LOGI(TAG, "GET /sensor");

    /* Lay ban sao snapshot du lieu duoi quyen mutex */
    sensor_data_t snapshot;
    xSemaphoreTake(s_sensor_mutex, portMAX_DELAY);
    snapshot = s_sensor_data;
    xSemaphoreGive(s_sensor_mutex);

    int64_t now_ms = get_current_time_ms();

    /* Truong hop 1: Chua tung doc thanh cong bat ky mau nao -> 5.03 Service Unavailable */
    if (!snapshot.has_valid_sample) {
        ESP_LOGW(TAG, "response=5.03 (No sensor data available yet)");

        otMessage *response = otCoapNewMessage(instance, NULL);
        if (response != NULL) {
            const char *err_json = "{\"error\":\"No sensor data available yet\"}";
            uint16_t    err_len  = (uint16_t)strlen(err_json);

            otError err = otCoapMessageInitResponse(response, aMessage,
                                                    resp_type,
                                                    OT_COAP_CODE_SERVICE_UNAVAILABLE);
            if (err == OT_ERROR_NONE) {
                err = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_JSON);
            }
            if (err == OT_ERROR_NONE) {
                err = otCoapMessageSetPayloadMarker(response);
            }
            if (err == OT_ERROR_NONE) {
                err = otMessageAppend(response, err_json, err_len);
            }
            if (err == OT_ERROR_NONE) {
                err = otCoapSendResponse(instance, response, aMessageInfo);
            }

            if (err != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "Failed to send 5.03 response: %d", err);
                otMessageFree(response);
            }
        }
        return;
    }

    /* Truong hop 2: Da co mau hop le -> Tra ve 2.05 CONTENT kem JSON */
    int64_t age_ms = now_ms - snapshot.last_success_ms;
    if (age_ms < 0) {
        age_ms = 0;
    }
    bool stale = (age_ms > SENSOR_STALE_THRESH_MS);

    char json_buf[256];
    int len = snprintf(json_buf, sizeof(json_buf),
                       "{\"node_id\":\"%s\",\"sensor\":\"DHT11\","
                       "\"temperature_c\":%.1f,\"humidity_pct\":%.1f,"
                       "\"sample_seq\":%" PRIu32 ",\"age_ms\":%" PRId64 ","
                       "\"last_read_ok\":%s,\"stale\":%s}",
                       SENSOR_NODE_ID,
                       snapshot.temperature_c,
                       snapshot.humidity_pct,
                       snapshot.sample_seq,
                       age_ms,
                       snapshot.last_read_ok ? "true" : "false",
                       stale ? "true" : "false");

    /* Kiem tra ket qua snprintf truoc khi su dung len */
    if (len < 0 || (size_t)len >= sizeof(json_buf)) {
        ESP_LOGE(TAG, "JSON buffer overflow or encoding error (len=%d)", len);

        otMessage *response = otCoapNewMessage(instance, NULL);
        if (response != NULL) {
            otError err = otCoapMessageInitResponse(response, aMessage,
                                                    resp_type,
                                                    OT_COAP_CODE_INTERNAL_ERROR);
            if (err == OT_ERROR_NONE) {
                err = otCoapSendResponse(instance, response, aMessageInfo);
            }
            if (err != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "Failed to send 5.00 response: %d", err);
                otMessageFree(response);
            }
        }
        return;
    }

    otMessage *response = otCoapNewMessage(instance, NULL);
    if (response != NULL) {
        otError err = otCoapMessageInitResponse(response, aMessage,
                                                resp_type,
                                                OT_COAP_CODE_CONTENT);
        if (err == OT_ERROR_NONE) {
            /* Bat buoc them Content-Format JSON truoc khi set payload marker */
            err = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_JSON);
        }
        if (err == OT_ERROR_NONE) {
            err = otCoapMessageSetPayloadMarker(response);
        }
        if (err == OT_ERROR_NONE) {
            err = otMessageAppend(response, json_buf, (uint16_t)len);
        }
        if (err == OT_ERROR_NONE) {
            err = otCoapSendResponse(instance, response, aMessageInfo);
        }

        if (err == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "response=2.05 sample_seq=%" PRIu32 " stale=%s",
                     snapshot.sample_seq, stale ? "true" : "false");
        } else {
            ESP_LOGE(TAG, "Failed to send 2.05 response: %d", err);
            otMessageFree(response);
        }
    }
}

static void coap_server_init(void)
{
    otInstance *instance = esp_openthread_get_instance();

    memset(&s_sensor_resource, 0, sizeof(s_sensor_resource));
    s_sensor_resource.mUriPath = "sensor";
    s_sensor_resource.mHandler = sensor_request_handler;
    s_sensor_resource.mContext = NULL;
    s_sensor_resource.mNext    = NULL;

    otCoapAddResource(instance, &s_sensor_resource);
    otCoapStart(instance, OT_DEFAULT_COAP_PORT);

    ESP_LOGI(TAG, "CoAP server started - port %d, resource: /sensor", OT_DEFAULT_COAP_PORT);
}

/* ========================================================================= */
/*  SRP Client Registration                                                  */
/* ========================================================================= */

#define NODE_SRP_NAME "sensor-01"
#define NODE_SRP_TYPE "_sensor._udp"

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

    otError err = otJoinerStart(instance, "S7K4M9", NULL, NULL, NULL, NULL, NULL, pair_join_callback, NULL);
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
    /* --- 1. Khoi tao he thong co ban --- */
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3, /* netif + task queue + radio */
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    /* --- 2. Khoi tao Mutex va Task lay mau DHT11 (chay doc lap) --- */
    s_sensor_mutex = xSemaphoreCreateMutex();
    assert(s_sensor_mutex != NULL);

    xTaskCreate(sensor_sampling_task, "sensor_task", 4096, NULL, 5, NULL);

    /* --- 3. Bat CLI console --- */
#if CONFIG_OPENTHREAD_CLI
    ot_console_start();
    ot_register_external_commands();
#endif

    /* --- 4. Khoi dong OpenThread (native radio tren C6) --- */
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

    /* --- 5. Cau hinh SED (Sleepy End Device), CoAP server & Auto-Reconnect --- */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();

    otLinkModeConfig sensor_mode = {
        .mRxOnWhenIdle = false,
        .mDeviceType   = false,
        .mNetworkData  = false,
    };
    otError sed_err = otThreadSetLinkMode(instance, sensor_mode);
    if (sed_err == OT_ERROR_NONE) {
        sed_err = otLinkSetPollPeriod(instance, SENSOR_POLL_PERIOD_MS);
    }
    if (sed_err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "SED configuration failed: error=%d", (int)sed_err);
        esp_openthread_lock_release();
        return;
    }
    ESP_LOGI(TAG, "SED configured: mode=-, pollperiod=%lu ms",
             (unsigned long)otLinkGetPollPeriod(instance));

    coap_server_init();

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

    /* --- 7. In banner khoi dong --- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  NODE SENSOR (CoAP DHT11 Server) READY!");
    ESP_LOGI(TAG, "  Sensor: DHT11 on GPIO%d", DHT_GPIO);
    ESP_LOGI(TAG, "  CoAP Resource: /sensor (GET only)");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Neu chua co dataset, gia nhap mang bang lenh CLI:");
    ESP_LOGI(TAG, "  ot dataset set active <hex-tu-Border-Router>");
    ESP_LOGI(TAG, "  ot ifconfig up");
    ESP_LOGI(TAG, "  ot thread start");
    ESP_LOGI(TAG, "");
}
