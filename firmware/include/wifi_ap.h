#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_ap_start(void);
bool wifi_ap_is_up(void);
const char *wifi_ap_ssid(void);
const char *wifi_ap_ip(void);

/** SoftAP esp_netif (NULL if not started). */
esp_netif_t *wifi_ap_netif(void);

#ifdef __cplusplus
}
#endif

