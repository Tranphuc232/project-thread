/*
 * SENSOR CLIENT MODULE - WROOM_HOST (Border Router / Gateway)
 *
 * Dinh ky gui CoAP GET den Sensor Node (ESP32-C6) qua SRP ("sensor-01"):
 *   _sensor._udp /sensor
 * Nhan, phan tich JSON va cap nhat du lieu nhiet do / do am.
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

#include "cJSON.h"
#include "sensor_client.h"
#include "device_registry.h"

#define TAG                 "SENSOR_CLIENT"
#define SENSOR_URI          "sensor"
#define SENSOR_PORT         5683
#define SENSOR_POLL_MS      10000
#define SENSOR_PAYLOAD_MAX  512
#define SENSOR_NODE_ID      "sensor_01"

/* ========================================================================= */
/*  Dinh nghia Queue Event & Du lieu trang thai                              */
/* ========================================================================= */

typedef enum {
    SENSOR_EVENT_RESPONSE,
    SENSOR_EVENT_TIMEOUT,
    SENSOR_EVENT_ERROR,
} sensor_event_type_t;

typedef struct {
    sensor_event_type_t type;
    otCoapCode          code;
    otError             error;
    int64_t             rx_time_ms;
    uint16_t            payload_len;
    char                payload[SENSOR_PAYLOAD_MAX];
} sensor_event_t;

static QueueHandle_t s_sensor_queue = NULL;
static QueueHandle_t s_snapshot_queue = NULL;

static sensor_client_data_t s_sensor_data = {
    .has_valid_sample = false,
    .temperature_c    = 0.0f,
    .humidity_pct     = 0.0f,
    .sample_seq       = 0,
    .age_ms           = 0,
    .last_rx_time_ms  = 0,
    .last_read_ok     = false,
    .stale            = false,
    .state            = SENSOR_STATE_NO_DATA,
};

/* ========================================================================= */
/*  Callback nhan phan hoi CoAP tu OpenThread                                */
/* ========================================================================= */

static void sensor_publish_event(const sensor_event_t *event)
{
    configASSERT(s_sensor_queue != NULL);
    BaseType_t ret = xQueueOverwrite(s_sensor_queue, event);
    configASSERT(ret == pdPASS);
    (void)ret;
}

static void sensor_response_handler(void *context,
                                   otMessage *message,
                                   const otMessageInfo *message_info,
                                   otError result)
{
    sensor_event_t event;
    memset(&event, 0, sizeof(event));
    event.rx_time_ms = esp_timer_get_time() / 1000;
    event.error = result;

    if (result != OT_ERROR_NONE) {
        event.type = (result == OT_ERROR_RESPONSE_TIMEOUT) ? SENSOR_EVENT_TIMEOUT : SENSOR_EVENT_ERROR;
        sensor_publish_event(&event);
        return;
    }

    if (message == NULL) {
        event.type = SENSOR_EVENT_ERROR;
        event.error = OT_ERROR_FAILED;
        sensor_publish_event(&event);
        return;
    }

    event.type = SENSOR_EVENT_RESPONSE;
    event.code = otCoapMessageGetCode(message);

    uint16_t offset = otMessageGetOffset(message);
    uint16_t total = otMessageGetLength(message);
    if (offset > total) {
        ESP_LOGE(TAG, "Invalid CoAP payload offset");
        event.type = SENSOR_EVENT_ERROR;
        event.error = OT_ERROR_PARSE;
        sensor_publish_event(&event);
        return;
    }
    uint16_t length = total - offset;
    if (length >= sizeof(event.payload)) {
        ESP_LOGE(TAG, "CoAP payload too large: %u bytes", (unsigned)length);
        event.type = SENSOR_EVENT_ERROR;
        event.error = OT_ERROR_PARSE;
        sensor_publish_event(&event);
        return;
    }
    uint16_t bytes_read = 0;
    if (length > 0) {
        bytes_read = otMessageRead(message, offset, event.payload, length);
    }
    if (bytes_read != length) {
        ESP_LOGE(TAG, "CoAP payload read incomplete: %u/%u bytes", (unsigned)bytes_read, (unsigned)length);
        event.type = SENSOR_EVENT_ERROR;
        event.error = OT_ERROR_PARSE;
        sensor_publish_event(&event);
        return;
    }
    event.payload[length] = '\0';
    event.payload_len = length;

    sensor_publish_event(&event);
}

/* ========================================================================= */
/*  Tao va gui CoAP GET (da giu OpenThread lock)                             */
/* ========================================================================= */

static otError sensor_send_get_locked(otInstance *instance)
{
    otMessageInfo message_info = {0};
    if (!device_registry_find_locked(instance, "sensor-01", "_sensor._udp", &message_info.mPeerAddr, &message_info.mPeerPort)) {
        ESP_LOGW(TAG, "SENSOR_NOT_DISCOVERED");
        return OT_ERROR_NOT_FOUND;
    }

    otMessage *message = otCoapNewMessage(instance, NULL);
    if (message == NULL) {
        ESP_LOGE(TAG, "otCoapNewMessage failed: no buffer");
        return OT_ERROR_NO_BUFS;
    }

    otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_GET);
    otCoapMessageGenerateToken(message, OT_COAP_DEFAULT_TOKEN_LENGTH);

    otError err = otCoapMessageAppendUriPathOptions(message, SENSOR_URI);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otCoapMessageAppendUriPathOptions failed: %d", err);
        otMessageFree(message);
        return err;
    }

    err = otCoapSendRequest(instance, message, &message_info, sensor_response_handler, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otCoapSendRequest failed: %d", err);
        otMessageFree(message);
        return err;
    }

    return OT_ERROR_NONE;
}

/* ========================================================================= */
/*  Phan tich JSON va quan ly trang thai du lieu                             */
/* ========================================================================= */

static void sensor_process_response(const sensor_event_t *event)
{
    if (event->type == SENSOR_EVENT_TIMEOUT) {
        s_sensor_data.state = SENSOR_STATE_TIMEOUT;
        ESP_LOGW(TAG, "SENSOR_TIMEOUT: %s", SENSOR_NODE_ID);
        return;
    }

    if (event->type == SENSOR_EVENT_ERROR) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        ESP_LOGE(TAG, "SENSOR_ERROR: %s (error code=%d)", SENSOR_NODE_ID, event->error);
        return;
    }

    if (event->code != OT_COAP_CODE_CONTENT) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        ESP_LOGW(TAG, "SENSOR_RESP_ERROR: %s (received CoAP code %d, expected 2.05)",
                 SENSOR_NODE_ID, event->code);
        return;
    }

    cJSON *root = cJSON_Parse(event->payload);
    if (root == NULL) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        ESP_LOGE(TAG, "SENSOR_RESP_ERROR: %s (failed to parse JSON)", SENSOR_NODE_ID);
        return;
    }

    cJSON *j_node_id = cJSON_GetObjectItem(root, "node_id");
    cJSON *j_temp    = cJSON_GetObjectItem(root, "temperature_c");
    cJSON *j_hum     = cJSON_GetObjectItem(root, "humidity_pct");
    cJSON *j_seq     = cJSON_GetObjectItem(root, "sample_seq");
    cJSON *j_age     = cJSON_GetObjectItem(root, "age_ms");
    cJSON *j_ok      = cJSON_GetObjectItem(root, "last_read_ok");
    cJSON *j_stale   = cJSON_GetObjectItem(root, "stale");

    bool valid = (j_node_id && cJSON_IsString(j_node_id) && strcmp(j_node_id->valuestring, SENSOR_NODE_ID) == 0) &&
                 (j_temp && cJSON_IsNumber(j_temp) && !isnan(j_temp->valuedouble) && !isinf(j_temp->valuedouble)) &&
                 (j_hum && cJSON_IsNumber(j_hum) && !isnan(j_hum->valuedouble) && !isinf(j_hum->valuedouble) &&
                  j_hum->valuedouble >= 0.0 && j_hum->valuedouble <= 100.0) &&
                 (j_seq && cJSON_IsNumber(j_seq) && j_seq->valuedouble >= 0.0) &&
                 (j_age && cJSON_IsNumber(j_age) && j_age->valuedouble >= 0.0) &&
                 (j_ok && cJSON_IsBool(j_ok)) &&
                 (j_stale && cJSON_IsBool(j_stale));

    if (!valid) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        ESP_LOGE(TAG, "SENSOR_RESP_ERROR: %s (invalid or missing JSON fields)", SENSOR_NODE_ID);
        cJSON_Delete(root);
        return;
    }

    float temp = (float)j_temp->valuedouble;
    float hum = (float)j_hum->valuedouble;
    uint32_t seq = (uint32_t)j_seq->valuedouble;
    int64_t age = (int64_t)j_age->valuedouble;
    bool last_read_ok = cJSON_IsTrue(j_ok);
    bool stale = cJSON_IsTrue(j_stale);

    sensor_client_state_t prev_state = s_sensor_data.state;

    /* Cap nhat mau du lieu hop le */
    s_sensor_data.has_valid_sample = true;
    s_sensor_data.temperature_c = temp;
    s_sensor_data.humidity_pct = hum;
    s_sensor_data.sample_seq = seq;
    s_sensor_data.age_ms = age;
    s_sensor_data.last_rx_time_ms = event->rx_time_ms;
    s_sensor_data.last_read_ok = last_read_ok;
    s_sensor_data.stale = stale;
    strncpy(s_sensor_data.node_id, j_node_id->valuestring, sizeof(s_sensor_data.node_id) - 1);

    if (!last_read_ok) {
        s_sensor_data.state = SENSOR_STATE_READ_ERROR;
        ESP_LOGW(TAG,
                 "SENSOR_READ_ERROR: %s seq=%" PRIu32
                 " stale=%s age_ms=%" PRId64,
                 SENSOR_NODE_ID, seq,
                 stale ? "true" : "false", age);
    } else if (stale) {
        s_sensor_data.state = SENSOR_STATE_STALE;
        ESP_LOGW(TAG,
                 "SENSOR_STALE: %s seq=%" PRIu32 " age_ms=%" PRId64,
                 SENSOR_NODE_ID, seq, age);
    } else {
        s_sensor_data.state = SENSOR_STATE_GOOD;

        if (prev_state == SENSOR_STATE_TIMEOUT ||
            prev_state == SENSOR_STATE_RESP_ERROR ||
            prev_state == SENSOR_STATE_READ_ERROR ||
            prev_state == SENSOR_STATE_STALE) {
            ESP_LOGI(TAG, "SENSOR_RECOVERED: %s", SENSOR_NODE_ID);
        }

        ESP_LOGI(TAG, "SENSOR_RX: %s temp=%.1f humidity=%.1f seq=%" PRIu32 " stale=false",
                 SENSOR_NODE_ID, temp, hum, seq);
    }

    cJSON_Delete(root);
}

/* ========================================================================= */
/*  Task quan ly chu ky lay mau (Poll Task)                                  */
/* ========================================================================= */

static void sensor_poll_task(void *pvParameters)
{
    otInstance *instance = NULL;
    while ((instance = esp_openthread_get_instance()) == NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool request_pending = false;
    TickType_t last_poll_tick = xTaskGetTickCount() - pdMS_TO_TICKS(SENSOR_POLL_MS);

    while (1) {
        TickType_t now_tick = xTaskGetTickCount();
        TickType_t elapsed_ticks = now_tick - last_poll_tick;
        TickType_t poll_period_ticks = pdMS_TO_TICKS(SENSOR_POLL_MS);

        if (elapsed_ticks >= poll_period_ticks) {
            last_poll_tick = now_tick;

            if (request_pending) {
                ESP_LOGW(TAG, "Previous request still pending, skipping this poll cycle");
            } else {
                bool in_network = false;
                if (esp_openthread_lock_acquire(portMAX_DELAY)) {
                    otDeviceRole role = otThreadGetDeviceRole(instance);
                    in_network = (role == OT_DEVICE_ROLE_CHILD ||
                                  role == OT_DEVICE_ROLE_ROUTER ||
                                  role == OT_DEVICE_ROLE_LEADER);

                    if (in_network) {
                        otError send_err = sensor_send_get_locked(instance);
                        if (send_err == OT_ERROR_NONE) {
                            request_pending = true;
                            ESP_LOGD(TAG, "Sent GET /sensor to sensor-01");
                        } else {
                            ESP_LOGE(TAG, "Failed to send GET: %d", send_err);
                        }
                    } else {
                        ESP_LOGD(TAG, "Host not attached to Thread network (role=%d), waiting", role);
                    }
                    esp_openthread_lock_release();
                }
            }
        }

        /* Cho su kien phan hoi tu queue (toi da 1 giay de kiem tra chu ky) */
        now_tick = xTaskGetTickCount();
        elapsed_ticks = now_tick - last_poll_tick;
        TickType_t wait_ticks = (elapsed_ticks < poll_period_ticks) ? (poll_period_ticks - elapsed_ticks) : 0;
        if (wait_ticks > pdMS_TO_TICKS(1000)) {
            wait_ticks = pdMS_TO_TICKS(1000);
        }

        sensor_event_t event;
        if (xQueueReceive(s_sensor_queue, &event, wait_ticks) == pdTRUE) {
            request_pending = false;
            sensor_process_response(&event);

            BaseType_t snapshot_ret = xQueueOverwrite(s_snapshot_queue, &s_sensor_data);
            configASSERT(snapshot_ret == pdPASS);
            (void)snapshot_ret;
        }
    }
}

/* ========================================================================= */
/*  Khoi tao module Sensor Client                                            */
/* ========================================================================= */

esp_err_t sensor_client_start(void)
{
    if (s_sensor_queue != NULL) {
        ESP_LOGW(TAG, "Sensor client already started");
        return ESP_ERR_INVALID_STATE;
    }

    s_sensor_queue = xQueueCreate(1, sizeof(sensor_event_t));
    if (s_sensor_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor event queue");
        return ESP_ERR_NO_MEM;
    }

    s_snapshot_queue = xQueueCreate(1, sizeof(sensor_client_data_t));
    if (s_snapshot_queue == NULL) {
        vQueueDelete(s_sensor_queue);
        s_sensor_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t snapshot_ret = xQueueOverwrite(s_snapshot_queue, &s_sensor_data);
    configASSERT(snapshot_ret == pdPASS);
    (void)snapshot_ret;

    BaseType_t ret = xTaskCreate(sensor_poll_task, "sensor_poll", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor poll task");
        vQueueDelete(s_snapshot_queue);
        s_snapshot_queue = NULL;
        vQueueDelete(s_sensor_queue);
        s_sensor_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sensor client module started (poll period: %d ms, target: sensor-01 via SRP)",
             SENSOR_POLL_MS);
    return ESP_OK;
}

esp_err_t sensor_client_get_snapshot(sensor_client_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_snapshot_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueuePeek(s_snapshot_queue, out, 0) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}
