#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tra cuu thong tin dia chi IPv6 va port cua mot thiet bi tu bang SRP Server.
 *
 * @note Ham nay phai duoc goi khi DA GIU OpenThread lock (caller quan ly lock).
 *
 * @param[in]  instance        Con tro otInstance cua OpenThread.
 * @param[in]  instance_label  Nhan instance (vd: "relay-01", "sensor-01", "dimmer-test-01").
 * @param[in]  service_type    Loai dich vu (vd: "_relay._udp", "_sensor._udp", "_dimmer._udp").
 * @param[out] out_addr        Con tro nhan dia chi IPv6 cua thiet bi.
 * @param[out] out_port        Con tro nhan port CoAP cua thiet bi.
 *
 * @return true neu tim thay ban ghi hop le (co dung 1 dia chi IPv6 va port != 0), false neu khong tim thay.
 */
bool device_registry_find_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    otIp6Address *out_addr,
    uint16_t     *out_port
);

/**
 * @brief Wrapper tuong thich tra cuu thong tin Node Dimmer ("dimmer-test-01", "_dimmer._udp").
 */
esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);

#ifdef __cplusplus
}
#endif
