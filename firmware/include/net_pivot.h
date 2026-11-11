#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enable L3 forward + NAPT: SoftAP clients (192.168.4.0/24) can reach
 * the USB-NCM host (typically 203.0.113.2) through the dongle.
 *
 * Call after both usb_net_start() and wifi_ap_start() succeed.
 */
esp_err_t net_pivot_start(void);

bool net_pivot_is_up(void);

/** Expected USB-side host address (DHCP first client). */
const char *net_pivot_host_ip(void);

#ifdef __cplusplus
}
#endif
