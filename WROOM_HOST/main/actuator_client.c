/*
 * ACTUATOR CLIENT MODULE - WROOM_HOST (Border Router / Gateway)
 *
 * Quan ly yeu cau dieu khien Relay va Dimmer qua CoAP Confirmable.
 * - Relay:  PUT /relay ("ON"/"OFF") hoac GET /relay
 * - Dimmer: PUT /light ("0".."100") hoac GET /light
 *
 * Cung cap cac ham enqueue bat dong bo va snapshot thread-safe co mutex.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

#include "actuator_client.h"
#include "device_registry.h"

#define TAG                     "ACTUATOR_CLIENT"

#define RELAY_URI               "relay"
#define DIMMER_URI              "light"
#define ACTUATOR_PORT           5683

#define ACTUATOR_PAYLOAD_MAX    64
#define ACTUATOR_CMD_QUEUE_LEN  8
#define ACTUATOR_TIMEOUT_WAIT_MS 12000

/* ========================================================================= */
/*  Dinh nghia Queue Event, Command & Bien trang thai                        */
/* ========================================================================= */

typedef struct {
    actuator_device_t device;
    bool              is_get;
    int               value;
} actuator_command_t;

typedef enum {
    ACTUATOR_EVT_RESPONSE,
    ACTUATOR_EVT_TIMEOUT,
    ACTUATOR_EVT_ERROR,
} actuator_event_type_t;

typedef struct {
    actuator_event_type_t type;
    otCoapCode            code;
    otError               error;
    int64_t               rx_time_ms;
    uint16_t              payload_len;
    char                  payload[ACTUATOR_PAYLOAD_MAX];
} actuator_event_t;

static QueueHandle_t     s_cmd_queue = NULL;
static QueueHandle_t     s_evt_queue = NULL;
static SemaphoreHandle_t s_snapshot_mutex = NULL;

static actuator_snapshot_t s_relay_snapshot = {
    .has_confirmed_value = false,
    .confirmed_value     = 0,
    .requested_value     = 0,
    .status              = ACTUATOR_UNKNOWN,
    .last_confirmed_ms   = 0,
};

static actuator_snapshot_t s_dimmer_snapshot = {
    .has_confirmed_value = false,
    .confirmed_value     = 0,
    .requested_value     = 0,
    .status              = ACTUATOR_UNKNOWN,
    .last_confirmed_ms   = 0,
};

/* ========================================================================= */
/*  Callback nhan phan hoi CoAP tu OpenThread                                */
/* ========================================================================= */

static void actuator_publish_event(const actuator_event_t *event)
{
    configASSERT(s_evt_queue != NULL);
    BaseType_t ret = xQueueOverwrite(s_evt_queue, event);
    configASSERT(ret == pdPASS);
    (void)ret;
}

static void actuator_response_handler(void *context,
                                     otMessage *message,
                                     const otMessageInfo *message_info,
                                     otError result)
{
    (void)context;
    (void)message_info;

    actuator_event_t event;
    memset(&event, 0, sizeof(event));
    event.rx_time_ms = esp_timer_get_time() / 1000;
    event.error = result;

    if (result != OT_ERROR_NONE) {
        event.type = (result == OT_ERROR_RESPONSE_TIMEOUT)
                         ? ACTUATOR_EVT_TIMEOUT
                         : ACTUATOR_EVT_ERROR;
        actuator_publish_event(&event);
        return;
    }

    if (message == NULL) {
        event.type = ACTUATOR_EVT_ERROR;
        event.error = OT_ERROR_FAILED;
        actuator_publish_event(&event);
        return;
    }

    event.type = ACTUATOR_EVT_RESPONSE;
    event.code = otCoapMessageGetCode(message);

    uint16_t offset = otMessageGetOffset(message);
    uint16_t total  = otMessageGetLength(message);
    if (offset > total) {
        ESP_LOGE(TAG, "Invalid CoAP payload offset");
        event.type = ACTUATOR_EVT_ERROR;
        event.error = OT_ERROR_PARSE;
        actuator_publish_event(&event);
        return;
    }

    uint16_t length = total - offset;
    if (length >= sizeof(event.payload)) {
        ESP_LOGE(TAG, "CoAP payload too large: %u bytes", (unsigned)length);
        event.type = ACTUATOR_EVT_ERROR;
        event.error = OT_ERROR_PARSE;
        actuator_publish_event(&event);
        return;
    }

    uint16_t bytes_read = 0;
    if (length > 0) {
        bytes_read = otMessageRead(message, offset, event.payload, length);
    }
    if (bytes_read != length) {
        ESP_LOGE(TAG, "CoAP payload read incomplete: %u/%u bytes", (unsigned)bytes_read, (unsigned)length);
        event.type = ACTUATOR_EVT_ERROR;
        event.error = OT_ERROR_PARSE;
        actuator_publish_event(&event);
        return;
    }

    event.payload[length] = '\0';
    event.payload_len = length;

    actuator_publish_event(&event);
}

/* ========================================================================= */
/*  Gui CoAP Request (da giu OpenThread lock)                                */
/* ========================================================================= */

static otError actuator_send_request_locked(otInstance *instance, const actuator_command_t *cmd)
{
    const char   *uri       = (cmd->device == ACTUATOR_RELAY) ? RELAY_URI : DIMMER_URI;
    otCoapCode    coap_code = cmd->is_get ? OT_COAP_CODE_GET : OT_COAP_CODE_PUT;
    otMessageInfo message_info = {0};

    /* Xac dinh dia chi dich va port */
    if (cmd->device == ACTUATOR_RELAY) {
        if (!device_registry_find_locked(instance, "relay-01", "_relay._udp", &message_info.mPeerAddr, &message_info.mPeerPort)) {
            ESP_LOGW(TAG, "RELAY_NOT_DISCOVERED");
            return OT_ERROR_NOT_FOUND;
        }
    } else if (cmd->device == ACTUATOR_DIMMER) {
        otIp6Address dimmer_addr;
        uint16_t     dimmer_port = 0;
        esp_err_t    reg_err = device_registry_find_dimmer_locked(instance, &dimmer_addr, &dimmer_port);
        if (reg_err != ESP_OK) {
            ESP_LOGW(TAG, "DIMMER_NOT_DISCOVERED: dimmer-test-01 not found in SRP Server registry");
            return OT_ERROR_NOT_FOUND;
        }
        message_info.mPeerAddr = dimmer_addr;
        message_info.mPeerPort = dimmer_port;
    } else {
        return OT_ERROR_INVALID_ARGS;
    }

    otMessage *message = otCoapNewMessage(instance, NULL);
    if (message == NULL) {
        ESP_LOGE(TAG, "otCoapNewMessage failed");
        return OT_ERROR_NO_BUFS;
    }

    otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, coap_code);
    otCoapMessageGenerateToken(message, OT_COAP_DEFAULT_TOKEN_LENGTH);

    otError err = otCoapMessageAppendUriPathOptions(message, uri);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to append URI %s: %d", uri, err);
        otMessageFree(message);
        return err;
    }

    if (!cmd->is_get) {
        char payload[32];
        int len = 0;
        if (cmd->device == ACTUATOR_RELAY) {
            len = snprintf(payload, sizeof(payload), "%s", cmd->value ? "ON" : "OFF");
        } else {
            len = snprintf(payload, sizeof(payload), "%d", cmd->value);
        }

        if (len < 0 || len >= sizeof(payload)) {
            ESP_LOGE(TAG, "Payload format error");
            otMessageFree(message);
            return OT_ERROR_FAILED;
        }

        err = otCoapMessageSetPayloadMarker(message);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to set payload marker: %d", err);
            otMessageFree(message);
            return err;
        }

        err = otMessageAppend(message, payload, (uint16_t)len);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to append payload: %d", err);
            otMessageFree(message);
            return err;
        }
    }

    err = otCoapSendRequest(instance, message, &message_info, actuator_response_handler, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otCoapSendRequest failed: %d", err);
        otMessageFree(message);
        return err;
    }

    char peer_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&message_info.mPeerAddr, peer_str, sizeof(peer_str));
    ESP_LOGI(TAG, "Sent %s %s/%s to [%s]:%u",
             cmd->is_get ? "GET" : "PUT",
             (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer",
             uri, peer_str, (unsigned)message_info.mPeerPort);
    return OT_ERROR_NONE;
}

/* ========================================================================= */
/*  Kiem tra va cap nhat ket qua vao Snapshot                                */
/* ========================================================================= */

static void actuator_verify_and_update(const actuator_command_t *cmd, const actuator_event_t *evt)
{
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    actuator_snapshot_t *snap = (cmd->device == ACTUATOR_RELAY)
                                    ? &s_relay_snapshot
                                    : &s_dimmer_snapshot;

    if (evt->type == ACTUATOR_EVT_TIMEOUT) {
        snap->status = ACTUATOR_TIMEOUT;
        ESP_LOGW(TAG, "Actuator %s request timeout",
                 (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer");
    } else if (evt->type == ACTUATOR_EVT_ERROR) {
        snap->status = ACTUATOR_ERROR;
        ESP_LOGE(TAG, "Actuator %s request error: %d",
                 (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer", evt->error);
    } else {
        bool match = false;
        int  confirmed_val = snap->confirmed_value;

        if (cmd->device == ACTUATOR_RELAY) {
            if (!cmd->is_get) {
                /* PUT relay: 2.04 CHANGED, payload ON / OFF khop lenh */
                if (evt->code == OT_COAP_CODE_CHANGED) {
                    if (cmd->value == 1 && (strcasecmp(evt->payload, "ON") == 0 || strcmp(evt->payload, "1") == 0)) {
                        match = true;
                        confirmed_val = 1;
                    } else if (cmd->value == 0 && (strcasecmp(evt->payload, "OFF") == 0 || strcmp(evt->payload, "0") == 0)) {
                        match = true;
                        confirmed_val = 0;
                    }
                }
            } else {
                /* GET relay: 2.05 CONTENT, payload ON / OFF */
                if (evt->code == OT_COAP_CODE_CONTENT) {
                    if (strcasecmp(evt->payload, "ON") == 0 || strcmp(evt->payload, "1") == 0) {
                        match = true;
                        confirmed_val = 1;
                    } else if (strcasecmp(evt->payload, "OFF") == 0 || strcmp(evt->payload, "0") == 0) {
                        match = true;
                        confirmed_val = 0;
                    }
                }
            }
        } else {
            /* Dimmer */
            if (!cmd->is_get) {
                /* PUT dimmer: 2.04 CHANGED, payload la so nguyen 0..100 khop lenh */
                if (evt->code == OT_COAP_CODE_CHANGED) {
                    char *endptr = NULL;
                    errno = 0;
                    long val = strtol(evt->payload, &endptr, 10);
                    if (errno == 0 && endptr != evt->payload) {
                        while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') endptr++;
                        if (*endptr == '\0' && val >= 0 && val <= 100 && (int)val == cmd->value) {
                            match = true;
                            confirmed_val = (int)val;
                        }
                    }
                }
            } else {
                /* GET dimmer: 2.05 CONTENT, payload la so nguyen 0..100 */
                if (evt->code == OT_COAP_CODE_CONTENT) {
                    char *endptr = NULL;
                    errno = 0;
                    long val = strtol(evt->payload, &endptr, 10);
                    if (errno == 0 && endptr != evt->payload) {
                        while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') endptr++;
                        if (*endptr == '\0' && val >= 0 && val <= 100) {
                            match = true;
                            confirmed_val = (int)val;
                        }
                    }
                }
            }
        }

        if (match) {
            snap->status = ACTUATOR_CONFIRMED;
            snap->has_confirmed_value = true;
            snap->confirmed_value = confirmed_val;
            snap->last_confirmed_ms = evt->rx_time_ms;
            ESP_LOGI(TAG, "Actuator %s CONFIRMED: value=%d (code=%d payload=\"%s\")",
                     (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer",
                     confirmed_val, evt->code, evt->payload);
        } else {
            snap->status = ACTUATOR_ERROR;
            ESP_LOGW(TAG, "Actuator %s response mismatch: code=%d payload=\"%s\"",
                     (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer",
                     evt->code, evt->payload);
        }
    }

    xSemaphoreGive(s_snapshot_mutex);
}

/* ========================================================================= */
/*  Task quan ly xu ly lenh Actuator (1 giao dich tai 1 thoi diem)           */
/* ========================================================================= */

static void actuator_task(void *pvParameters)
{
    (void)pvParameters;

    otInstance *instance = NULL;
    while ((instance = esp_openthread_get_instance()) == NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while (1) {
        actuator_command_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 1. Cap nhat snapshot: PENDING, requested_value */
        if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
            actuator_snapshot_t *snap = (cmd.device == ACTUATOR_RELAY)
                                            ? &s_relay_snapshot
                                            : &s_dimmer_snapshot;
            snap->status = ACTUATOR_PENDING;
            if (!cmd.is_get) {
                snap->requested_value = cmd.value;
            }
            xSemaphoreGive(s_snapshot_mutex);
        }

        /* 2. Kiem tra ket noi mang va gui CoAP Request */
        bool sent = false;
        if (esp_openthread_lock_acquire(portMAX_DELAY)) {
            otDeviceRole role = otThreadGetDeviceRole(instance);
            bool in_network = (role == OT_DEVICE_ROLE_CHILD ||
                               role == OT_DEVICE_ROLE_ROUTER ||
                               role == OT_DEVICE_ROLE_LEADER);
            if (in_network) {
                otError send_err = actuator_send_request_locked(instance, &cmd);
                if (send_err == OT_ERROR_NONE) {
                    sent = true;
                }
            } else {
                ESP_LOGW(TAG, "Host not attached to Thread network (role=%d), cannot send", role);
            }
            esp_openthread_lock_release();
        }

        if (!sent) {
            if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
                actuator_snapshot_t *snap = (cmd.device == ACTUATOR_RELAY)
                                                ? &s_relay_snapshot
                                                : &s_dimmer_snapshot;
                snap->status = ACTUATOR_ERROR;
                xSemaphoreGive(s_snapshot_mutex);
            }
            continue;
        }

        /* 3. Cho phan hoi tu event queue */
        actuator_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(ACTUATOR_TIMEOUT_WAIT_MS)) == pdTRUE) {
            actuator_verify_and_update(&cmd, &evt);
        } else {
            actuator_event_t timeout_evt = {
                .type  = ACTUATOR_EVT_TIMEOUT,
                .error = OT_ERROR_RESPONSE_TIMEOUT,
            };
            actuator_verify_and_update(&cmd, &timeout_evt);
        }
    }
}

/* ========================================================================= */
/*  Public APIs                                                              */
/* ========================================================================= */

esp_err_t actuator_client_start(void)
{
    if (s_cmd_queue != NULL) {
        ESP_LOGW(TAG, "Actuator client already started");
        return ESP_ERR_INVALID_STATE;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create snapshot mutex");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(ACTUATOR_CMD_QUEUE_LEN, sizeof(actuator_command_t));
    if (s_cmd_queue == NULL) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_evt_queue = xQueueCreate(1, sizeof(actuator_event_t));
    if (s_evt_queue == NULL) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_relay_snapshot, 0, sizeof(s_relay_snapshot));
    s_relay_snapshot.status = ACTUATOR_UNKNOWN;

    memset(&s_dimmer_snapshot, 0, sizeof(s_dimmer_snapshot));
    s_dimmer_snapshot.status = ACTUATOR_UNKNOWN;

    BaseType_t ret = xTaskCreate(actuator_task, "actuator_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create actuator task");
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Actuator client module started");
    return ESP_OK;
}

esp_err_t actuator_set_relay(bool on)
{
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    actuator_command_t cmd = {
        .device = ACTUATOR_RELAY,
        .is_get = false,
        .value  = on ? 1 : 0,
    };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping relay set command");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t actuator_set_brightness(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    actuator_command_t cmd = {
        .device = ACTUATOR_DIMMER,
        .is_get = false,
        .value  = percent,
    };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping dimmer set command");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t actuator_read_relay(void)
{
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    actuator_command_t cmd = {
        .device = ACTUATOR_RELAY,
        .is_get = true,
        .value  = 0,
    };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping relay read command");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t actuator_read_brightness(void)
{
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    actuator_command_t cmd = {
        .device = ACTUATOR_DIMMER,
        .is_get = true,
        .value  = 0,
    };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping dimmer read command");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t actuator_get_snapshot(actuator_device_t device, actuator_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_snapshot_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (device == ACTUATOR_RELAY) {
        *out = s_relay_snapshot;
    } else if (device == ACTUATOR_DIMMER) {
        *out = s_dimmer_snapshot;
    } else {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

