#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up USB NCM gadget + DHCP server at RNDISSTORAGE_IP_ADDR. */
esp_err_t usb_net_start(void);

/** USB NCM esp_netif (NULL if not started). */
esp_netif_t *usb_net_netif(void);

#ifdef __cplusplus
}
#endif

