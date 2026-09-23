#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up USB NCM gadget + DHCP server at RNDISSTORAGE_IP_ADDR. */
esp_err_t usb_net_start(void);

#ifdef __cplusplus
}
#endif
