/*
 * DEVICE REGISTRY MODULE - WROOM_HOST
 *
 * Tra cuu thiet bi tu bang SRP Server noi bo tren Border Router / Gateway.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"

#include "device_registry.h"

#define TAG "DEV_REGISTRY"

bool device_registry_find_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    otIp6Address *out_addr,
    uint16_t     *out_port)
{
    if (instance == NULL || instance_label == NULL || service_type == NULL || out_addr == NULL || out_port == NULL) {
        return false;
    }

    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        /* Bo qua host da bi xoa */
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        const otSrpServerService *service = NULL;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            /* Bo qua service da bi xoa */
            if (otSrpServerServiceIsDeleted(service)) {
                continue;
            }

            const char *label = otSrpServerServiceGetInstanceLabel(service);
            const char *service_name = otSrpServerServiceGetServiceName(service);

            /* So khop nhan instance */
            if (label == NULL || strcasecmp(label, instance_label) != 0) {
                continue;
            }

            /* So khop loai dich vu (kem domain SRP cua server) */
            if (service_name == NULL || strstr(service_name, service_type) == NULL) {
                continue;
            }

            /* Kiem tra port khac 0 */
            uint16_t port = otSrpServerServiceGetPort(service);
            if (port == 0) {
                continue;
            }

            /* Kiem tra dia chi: yeu cau ban ghi co dung 1 dia chi IPv6 */
            uint8_t addr_num = 0;
            const otIp6Address *addrs = otSrpServerHostGetAddresses(host, &addr_num);
            if (addrs == NULL || addr_num != 1) {
                ESP_LOGW(TAG, "Host '%s' has %u addresses (expected 1)",
                         otSrpServerHostGetFullName(host), (unsigned)addr_num);
                continue;
            }

            *out_addr = addrs[0];
            *out_port = port;

            char ip_str[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(&addrs[0], ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Found service via SRP: instance='%s' type='%s' addr=[%s]:%u",
                     label, service_name, ip_str, (unsigned)port);

            return true;
        }
    }

    return false;
}

esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    if (device_registry_find_locked(instance, "dimmer-test-01", "_dimmer._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
