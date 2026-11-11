#include "status_display.h"
#include "board_config.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tft";
static spi_device_handle_t s_spi = NULL;
static ui_status_t s_status = UI_BOOTING;
static const char *s_backend = "-";
static uint64_t s_total = 0, s_used = 0, s_free = 0;
static bool s_ready = false;
static uint16_t *s_fb = NULL;  // RGB565 framebuffer 160x80

/*
 * LilyGO T-Dongle-S3 ST7735 160x80 landscape (MADCTL MX|MV|BGR = 0x68).
 * Offsets swap vs portrait: portrait is col=26,row=1 → landscape col=1,row=26.
 * (st7789py / TFT_eSPI GREENTAB160x80 / Adafruit INITR_MINI160x80 rot 3)
 */
static const uint16_t COL_START = 1;
static const uint16_t ROW_START = 26;

static const uint8_t FONT_5X7[96][5] = {
#include "font5x7.inc"
};

static void tft_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_TFT_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_data(const void *data, size_t len)
{
    if (!len) {
        return;
    }
    gpio_set_level(PIN_TFT_DC, 1);
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        size_t chunk = len > 1024 ? 1024 : len;
        spi_transaction_t t = {.length = chunk * 8, .tx_buffer = p};
        spi_device_polling_transmit(s_spi, &t);
        p += chunk;
        len -= chunk;
    }
}

static void tft_data8(uint8_t v) { tft_data(&v, 1); }

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    x0 = (uint16_t)(x0 + COL_START);
    x1 = (uint16_t)(x1 + COL_START);
    y0 = (uint16_t)(y0 + ROW_START);
    y1 = (uint16_t)(y1 + ROW_START);
    uint8_t buf[4];
    tft_cmd(0x2A);
    buf[0] = (uint8_t)(x0 >> 8);
    buf[1] = (uint8_t)(x0 & 0xFF);
    buf[2] = (uint8_t)(x1 >> 8);
    buf[3] = (uint8_t)(x1 & 0xFF);
    tft_data(buf, 4);
    tft_cmd(0x2B);
    buf[0] = (uint8_t)(y0 >> 8);
    buf[1] = (uint8_t)(y0 & 0xFF);
    buf[2] = (uint8_t)(y1 >> 8);
    buf[3] = (uint8_t)(y1 & 0xFF);
    tft_data(buf, 4);
    tft_cmd(0x2C);
}

static void tft_push_fb(void)
{
    // Convert to big-endian on the wire for ST7735
    static uint8_t line[160 * 2];
    tft_set_window(0, 0, 159, 79);
    gpio_set_level(PIN_TFT_DC, 1);
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < 160; x++) {
            uint16_t c = s_fb[y * 160 + x];
            line[x * 2] = (uint8_t)(c >> 8);
            line[x * 2 + 1] = (uint8_t)(c & 0xFF);
        }
        spi_transaction_t t = {.length = 160 * 2 * 8, .tx_buffer = line};
        spi_device_polling_transmit(s_spi, &t);
    }
}

static void fb_clear(uint16_t color)
{
    for (int i = 0; i < 160 * 80; i++) {
        s_fb[i] = color;
    }
}

static void fb_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= 160 || y >= 80) {
        return;
    }
    s_fb[y * 160 + x] = color;
}

static void fb_char(int x, int y, char c, uint16_t fg)
{
    if (c < 32 || c > 127) {
        c = '?';
    }
    const uint8_t *g = FONT_5X7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                fb_pixel(x + col, y + row, fg);
            }
        }
    }
}

static void fb_text(int x, int y, const char *s, uint16_t fg)
{
    while (*s && x < 155) {
        fb_char(x, y, *s++, fg);
        x += 6;
    }
}

static void format_bytes(uint64_t bytes, char *out, size_t out_len)
{
    if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.2fMB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1fKB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%lluB", (unsigned long long)bytes);
    }
}

static const char *status_text(ui_status_t s)
{
    switch (s) {
        case UI_READY:
            return "Ready";
        case UI_FAILURE:
            return "Failure";
        case UI_TRANSFERRING:
            return "Xfer";
        case UI_BUSY:
            return "Busy";
        default:
            return "Boot";
    }
}

static uint16_t status_color(ui_status_t s)
{
    switch (s) {
        case UI_READY:
            return 0x07E0;
        case UI_FAILURE:
            return 0xF800;
        case UI_TRANSFERRING:
            return 0xFFE0;
        default:
            return 0xFFFF;
    }
}

static void st7735_init_mini160x80(void)
{
    // Based on Adafruit INITR_MINI160x80 / LilyGO factory sequence.
    tft_cmd(0x01);  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    tft_cmd(0x11);  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(255));

    tft_cmd(0xB1);  // FRMCTR1
    {
        uint8_t d[] = {0x01, 0x2C, 0x2D};
        tft_data(d, 3);
    }
    tft_cmd(0xB2);  // FRMCTR2
    {
        uint8_t d[] = {0x01, 0x2C, 0x2D};
        tft_data(d, 3);
    }
    tft_cmd(0xB3);  // FRMCTR3
    {
        uint8_t d[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
        tft_data(d, 6);
    }
    tft_cmd(0xB4);  // INVCTR
    tft_data8(0x07);

    tft_cmd(0xC0);  // PWCTR1
    {
        uint8_t d[] = {0xA2, 0x02, 0x84};
        tft_data(d, 3);
    }
    tft_cmd(0xC1);
    tft_data8(0xC5);
    tft_cmd(0xC2);
    {
        uint8_t d[] = {0x0A, 0x00};
        tft_data(d, 2);
    }
    tft_cmd(0xC3);
    {
        uint8_t d[] = {0x8A, 0x2A};
        tft_data(d, 2);
    }
    tft_cmd(0xC4);
    {
        uint8_t d[] = {0x8A, 0xEE};
        tft_data(d, 2);
    }
    tft_cmd(0xC5);  // VMCTR1
    tft_data8(0x0E);

    tft_cmd(0x21);  // INVON (IPS / greentab 160x80)

    // Landscape: MX | MV | BGR  (Adafruit MINI160x80 setRotation(3))
    tft_cmd(0x36);  // MADCTL
    tft_data8(0x68);

    tft_cmd(0x3A);  // COLMOD 16-bit
    tft_data8(0x05);

    tft_cmd(0xE0);  // GMCTRP1
    {
        uint8_t d[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                       0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
        tft_data(d, 16);
    }
    tft_cmd(0xE1);  // GMCTRN1
    {
        uint8_t d[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                       0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
        tft_data(d, 16);
    }

    tft_cmd(0x13);  // NORON
    vTaskDelay(pdMS_TO_TICKS(10));
    tft_cmd(0x29);  // DISPON
    vTaskDelay(pdMS_TO_TICKS(100));
}

bool status_display_begin(void)
{
    s_fb = heap_caps_malloc(160 * 80 * sizeof(uint16_t), MALLOC_CAP_DEFAULT);
    if (!s_fb) {
        ESP_LOGE(TAG, "no FB memory");
        return false;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_DC) | (1ULL << PIN_TFT_RST) | (1ULL << PIN_TFT_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_TFT_BL, 1);  // off (active-LOW)

    gpio_set_level(PIN_TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_TFT_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 160 * 2 + 16,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return false;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 26 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TFT_CS,
        .queue_size = 1,
        .flags = 0,
    };
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return false;
    }

    st7735_init_mini160x80();
    gpio_set_level(PIN_TFT_BL, 0);  // backlight on
    s_ready = true;
    status_display_refresh();
    ESP_LOGI(TAG, "display ready");
    return true;
}

void status_display_set_status(ui_status_t status)
{
    if (s_status == status) {
        return;
    }
    s_status = status;
    // Full SPI framebuffer push blocks the HTTP worker; skip during transfers.
    if (status == UI_TRANSFERRING) {
        return;
    }
    status_display_refresh();
}

void status_display_set_backend(const char *backend)
{
    s_backend = backend ? backend : "-";
    status_display_refresh();
}

void status_display_set_storage(uint64_t total, uint64_t used, uint64_t free_bytes)
{
    s_total = total;
    s_used = used;
    s_free = free_bytes;
    status_display_refresh();
}

void status_display_refresh(void)
{
    if (!s_ready || !s_fb) {
        return;
    }
    fb_clear(0x0000);
    fb_text(2, 2, "RNDISStorage", 0x07FF);
    fb_text(108, 2, s_backend, 0xFFFF);
    fb_text(2, 14, "Status:", 0xFFFF);
    fb_text(50, 14, status_text(s_status), status_color(s_status));

    char line[48];
    snprintf(line, sizeof(line), "USB %s", RNDISSTORAGE_IP_ADDR);
    fb_text(2, 26, line, 0xFFE0);
    snprintf(line, sizeof(line), "AP  %s", RNDISSTORAGE_WIFI_IP_ADDR);
    fb_text(2, 37, line, 0x07FF);

    char tb[16], ub[16], fr[16];
    format_bytes(s_total, tb, sizeof(tb));
    format_bytes(s_used, ub, sizeof(ub));
    format_bytes(s_free, fr, sizeof(fr));
    snprintf(line, sizeof(line), "T:%s U:%s", tb, ub);
    fb_text(2, 52, line, 0xFFFF);
    snprintf(line, sizeof(line), "Free:%s", fr);
    fb_text(2, 63, line, 0xFFFF);

    tft_push_fb();
}
