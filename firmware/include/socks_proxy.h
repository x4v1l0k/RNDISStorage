#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SOCKS5 CONNECT proxy on SoftAP (default port 1080). Works even if the
 *  Wi-Fi client has no default route — operator uses --socks5 192.168.4.1:1080.
 */
esp_err_t socks_proxy_start(void);
bool socks_proxy_is_up(void);
int socks_proxy_port(void);

#ifdef __cplusplus
}
#endif
