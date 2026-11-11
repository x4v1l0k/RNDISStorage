#pragma once

// LILYGO TTGO T-Dongle-S3 — ESP-IDF build (USB NCM + SoftAP, no MSC).

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdint.h>

static const int PIN_LED_DATA = 40;
static const int PIN_LED_CLOCK = 39;
static const int PIN_BUTTON = 0;

static const int PIN_TFT_CS = 4;
static const int PIN_TFT_MOSI = 3;
static const int PIN_TFT_SCLK = 5;
static const int PIN_TFT_DC = 2;
static const int PIN_TFT_RST = 1;
static const int PIN_TFT_BL = 38;

static const int PIN_SD_D0 = 14;
static const int PIN_SD_D1 = 17;
static const int PIN_SD_D2 = 21;
static const int PIN_SD_D3 = 18;
static const int PIN_SD_CLK = 12;
static const int PIN_SD_CMD = 16;

#ifndef RNDISSTORAGE_DEVICE_NAME
#define RNDISSTORAGE_DEVICE_NAME "TTGO-T-Dongle-S3"
#endif

#ifndef RNDISSTORAGE_PROTOCOL_VERSION
#define RNDISSTORAGE_PROTOCOL_VERSION "2.5"
#endif

#ifndef RNDISSTORAGE_SOCKS_PORT
#define RNDISSTORAGE_SOCKS_PORT 1080
#endif

/** USB NCM dongle IP (TEST-NET-3). */
#ifndef RNDISSTORAGE_IP_ADDR
#define RNDISSTORAGE_IP_ADDR "203.0.113.1"
#endif

/** USB NCM host IP assigned by on-dongle DHCP (first client). */
#ifndef RNDISSTORAGE_HOST_IP_ADDR
#define RNDISSTORAGE_HOST_IP_ADDR "203.0.113.2"
#endif

/** SoftAP IP — different subnet from USB NCM to avoid dual-netif clash. */
#ifndef RNDISSTORAGE_WIFI_IP_ADDR
#define RNDISSTORAGE_WIFI_IP_ADDR "192.168.4.1"
#endif

#ifndef RNDISSTORAGE_WIFI_SSID
#define RNDISSTORAGE_WIFI_SSID "RNDISStorage"
#endif

#ifndef RNDISSTORAGE_WIFI_PASS
#define RNDISSTORAGE_WIFI_PASS "rndis-lab"
#endif

#ifndef RNDISSTORAGE_WIFI_CHANNEL
#define RNDISSTORAGE_WIFI_CHANNEL 6
#endif

#ifndef RNDISSTORAGE_HTTP_PORT
#define RNDISSTORAGE_HTTP_PORT 80
#endif

#ifndef RNDISSTORAGE_MAX_PATH
#define RNDISSTORAGE_MAX_PATH 240
#endif

#ifndef RNDISSTORAGE_MAX_FILE_SIZE
#define RNDISSTORAGE_MAX_FILE_SIZE (512ULL * 1024ULL * 1024ULL)
#endif

/** I/O chunk for HTTP upload/download (heap). Keep modest: 2 parallel
 *  transfers + TCP/Wi‑Fi on ESP32-S3 without PSRAM; 32KiB×N caused OOM. */
#ifndef RNDISSTORAGE_IO_CHUNK
#define RNDISSTORAGE_IO_CHUNK (8 * 1024)
#endif

#define SD_MOUNT_POINT "/sdcard"

#endif
