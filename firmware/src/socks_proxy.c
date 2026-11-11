#include "socks_proxy.h"
#include "board_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#ifndef RNDISSTORAGE_SOCKS_PORT
#define RNDISSTORAGE_SOCKS_PORT 1080
#endif

#define RELAY_BUF 2048

static const char *TAG = "socks";
static bool s_up = false;

bool socks_proxy_is_up(void)
{
    return s_up;
}

int socks_proxy_port(void)
{
    return RNDISSTORAGE_SOCKS_PORT;
}

static int recv_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        int r = recv(fd, p + got, n - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static void relay_bidirectional(int a, int b)
{
    char buf[RELAY_BUF];
    fd_set rfds;
    int maxfd = a > b ? a : b;
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(a, &rfds);
        FD_SET(b, &rfds);
        struct timeval tv = {.tv_sec = 120, .tv_usec = 0};
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n <= 0) {
            break;
        }
        if (FD_ISSET(a, &rfds)) {
            int r = recv(a, buf, sizeof(buf), 0);
            if (r <= 0 || send(b, buf, r, 0) != r) {
                break;
            }
        }
        if (FD_ISSET(b, &rfds)) {
            int r = recv(b, buf, sizeof(buf), 0);
            if (r <= 0 || send(a, buf, r, 0) != r) {
                break;
            }
        }
    }
}

static void handle_client(int client)
{
    uint8_t buf[512];

    // greeting: VER NMETHODS METHODS...
    if (recv_exact(client, buf, 2) != 0) {
        goto done;
    }
    if (buf[0] != 0x05) {
        goto done;
    }
    uint8_t nmethods = buf[1];
    if (nmethods == 0 || recv_exact(client, buf, nmethods) != 0) {
        goto done;
    }
    // no auth
    uint8_t method_resp[2] = {0x05, 0x00};
    if (send(client, method_resp, 2, 0) != 2) {
        goto done;
    }

    // request: VER CMD RSV ATYP ...
    if (recv_exact(client, buf, 4) != 0) {
        goto done;
    }
    if (buf[0] != 0x05 || buf[1] != 0x01 /* CONNECT */) {
        uint8_t fail[10] = {0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
        send(client, fail, sizeof(fail), 0);
        goto done;
    }

    uint8_t atyp = buf[3];
    char host[256];
    uint16_t port = 0;
    memset(host, 0, sizeof(host));

    if (atyp == 0x01) {  // IPv4
        if (recv_exact(client, buf, 4) != 0) {
            goto done;
        }
        snprintf(host, sizeof(host), "%u.%u.%u.%u", buf[0], buf[1], buf[2], buf[3]);
    } else if (atyp == 0x03) {  // domain
        if (recv_exact(client, buf, 1) != 0) {
            goto done;
        }
        uint8_t len = buf[0];
        if (len == 0 || (size_t)len >= sizeof(host) || recv_exact(client, host, len) != 0) {
            goto done;
        }
        host[len] = '\0';
    } else {
        uint8_t fail[10] = {0x05, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
        send(client, fail, sizeof(fail), 0);
        goto done;
    }

    if (recv_exact(client, buf, 2) != 0) {
        goto done;
    }
    port = (uint16_t)((buf[0] << 8) | buf[1]);

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_aton(host, &dest.sin_addr) == 0) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portstr[8];
        snprintf(portstr, sizeof(portstr), "%u", port);
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
            uint8_t fail[10] = {0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
            send(client, fail, sizeof(fail), 0);
            goto done;
        }
        memcpy(&dest, res->ai_addr, sizeof(dest));
        freeaddrinfo(res);
    }

    int remote = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (remote < 0) {
        uint8_t fail[10] = {0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
        send(client, fail, sizeof(fail), 0);
        goto done;
    }
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(remote, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(remote, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGW(TAG, "CONNECT %s:%u failed errno=%d", host, port, errno);
        uint8_t fail[10] = {0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
        send(client, fail, sizeof(fail), 0);
        close(remote);
        goto done;
    }

    uint8_t ok[10] = {0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    if (send(client, ok, sizeof(ok), 0) != sizeof(ok)) {
        close(remote);
        goto done;
    }

    ESP_LOGI(TAG, "relay → %s:%u", host, port);
    // clear timeouts for long transfers
    tv.tv_sec = 0;
    setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(remote, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    relay_bidirectional(client, remote);
    close(remote);

done:
    close(client);
}

static void socks_client_task(void *arg)
{
    int client = (int)(intptr_t)arg;
    handle_client(client);
    vTaskDelete(NULL);
}

static void socks_server_task(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RNDISSTORAGE_SOCKS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen failed errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    s_up = true;
    ESP_LOGI(TAG, "SOCKS5 on *:%d — curl --socks5 %s:%d http://%s:1234/", RNDISSTORAGE_SOCKS_PORT,
             RNDISSTORAGE_WIFI_IP_ADDR, RNDISSTORAGE_SOCKS_PORT, RNDISSTORAGE_HOST_IP_ADDR);

    while (1) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int client = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (client < 0) {
            continue;
        }
        // One task per connection keeps httpd/USB responsive.
        if (xTaskCreate(socks_client_task, "socks_c", 6144, (void *)(intptr_t)client, 5, NULL) != pdPASS) {
            close(client);
        }
    }
}

esp_err_t socks_proxy_start(void)
{
    if (s_up) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(socks_server_task, "socks", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
