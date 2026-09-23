#pragma once

// LILYGO TTGO T-Dongle-S3 — ESP-IDF build (USB NCM gadget, no SoftAP, no MSC).

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
#define RNDISSTORAGE_PROTOCOL_VERSION "2.1"
#endif

#ifndef RNDISSTORAGE_IP_ADDR
#define RNDISSTORAGE_IP_ADDR "203.0.113.1"
#endif

#ifndef RNDISSTORAGE_HTTP_PORT
#define RNDISSTORAGE_HTTP_PORT 80
#endif

#ifndef RNDISSTORAGE_MAX_PATH
#define RNDISSTORAGE_MAX_PATH 200
#endif

#ifndef RNDISSTORAGE_MAX_FILE_SIZE
#define RNDISSTORAGE_MAX_FILE_SIZE (512ULL * 1024ULL * 1024ULL)
#endif

#define SD_MOUNT_POINT "/sdcard"

#endif
