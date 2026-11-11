#include "board_config.h"
#include "http_webdav.h"
#include "net_pivot.h"
#include "socks_proxy.h"
#include "status_display.h"
#include "storage.h"
#include "usb_net.h"
#include "wifi_ap.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void led_byte(uint8_t v)
{
    for (int i = 7; i >= 0; --i) {
        gpio_set_level(PIN_LED_DATA, (v >> i) & 1);
        gpio_set_level(PIN_LED_CLOCK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LED_CLOCK, 0);
        esp_rom_delay_us(1);
    }
}

static void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 4; i++) {
        led_byte(0x00);
    }
    led_byte(0xE0 | 0x1F);
    led_byte(b);
    led_byte(g);
    led_byte(r);
    for (int i = 0; i < 4; i++) {
        led_byte(0xFF);
    }
}

void app_main(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED_DATA) | (1ULL << PIN_LED_CLOCK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_direction(PIN_BUTTON, GPIO_MODE_INPUT);
    gpio_pullup_en(PIN_BUTTON);

    // Magenta = firmware running (before USB). If this flickers → panic loop.
    led_rgb(48, 0, 48);
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t usb_err = usb_net_start();
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "USB NCM failed: %s", esp_err_to_name(usb_err));
        led_rgb(64, 0, 0);
    } else {
        led_rgb(0, 48, 0);
        ESP_LOGI(TAG, "USB NCM ready");
    }

    // Let Windows finish binding UsbNcm before SPI/SD/Wi-Fi traffic.
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_err_t wifi_err = wifi_ap_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP failed: %s — USB-only control still available", esp_err_to_name(wifi_err));
    }

    esp_err_t pivot_err = ESP_ERR_INVALID_STATE;
    if (usb_err == ESP_OK && wifi_err == ESP_OK) {
        pivot_err = net_pivot_start();
        if (pivot_err != ESP_OK) {
            ESP_LOGW(TAG, "pivot/NAPT failed: %s — SoftAP still serves UI, no host bridge",
                     esp_err_to_name(pivot_err));
        }
    }

    esp_err_t socks_err = ESP_FAIL;
    if (wifi_err == ESP_OK) {
        socks_err = socks_proxy_start();
        if (socks_err != ESP_OK) {
            ESP_LOGW(TAG, "SOCKS5 failed: %s", esp_err_to_name(socks_err));
        }
    }

    if (!status_display_begin()) {
        ESP_LOGW(TAG, "display init failed — continuing");
    } else {
        status_display_set_backend(usb_err == ESP_OK ? "NCM" : "USB-ERR");
        status_display_set_status(usb_err == ESP_OK ? UI_BOOTING : UI_FAILURE);
    }

    bool sd_ok = storage_begin();
    if (!sd_ok) {
        ESP_LOGE(TAG, "SD mount failed");
        status_display_set_backend("NO-SD");
        status_display_set_status(UI_FAILURE);
    } else {
        storage_info_t si = storage_info();
        status_display_set_storage(si.total_bytes, si.used_bytes, si.free_bytes);
        if (pivot_err == ESP_OK) {
            status_display_set_backend("PIVOT");
        } else if (wifi_err == ESP_OK) {
            status_display_set_backend("SD+AP");
        } else {
            status_display_set_backend("SD");
        }
    }

    // HTTP binds all interfaces: USB NCM + SoftAP.
    esp_err_t http_err = ESP_FAIL;
    if (usb_err == ESP_OK || wifi_err == ESP_OK) {
        http_err = http_webdav_start();
        if (http_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP failed: %s", esp_err_to_name(http_err));
        }
    }

    if (http_err == ESP_OK && sd_ok && (usb_err == ESP_OK || wifi_err == ESP_OK)) {
        status_display_set_status(UI_READY);
        led_rgb(0, 48, 0);
    } else if (usb_err != ESP_OK && wifi_err != ESP_OK) {
        status_display_set_status(UI_FAILURE);
        led_rgb(64, 0, 0);
    } else {
        status_display_set_status(UI_FAILURE);
        led_rgb(48, 24, 0);
    }

    ESP_LOGI(TAG, "boot done USB=%s WiFi=%s pivot=%s HTTP=%s SD=%s", esp_err_to_name(usb_err),
             esp_err_to_name(wifi_err), esp_err_to_name(pivot_err), esp_err_to_name(http_err),
             sd_ok ? "OK" : "FAIL");
    if (socks_err == ESP_OK) {
        ESP_LOGI(TAG, "operator: SoftAP → curl --socks5 %s:%d http://%s:1234/", RNDISSTORAGE_WIFI_IP_ADDR,
                 RNDISSTORAGE_SOCKS_PORT, RNDISSTORAGE_HOST_IP_ADDR);
    } else if (pivot_err == ESP_OK) {
        ESP_LOGI(TAG, "operator: join SoftAP → ssh user@%s  (host USB IP)", RNDISSTORAGE_HOST_IP_ADDR);
    }

    bool last_btn = true;
    while (1) {
        bool btn = gpio_get_level(PIN_BUTTON);
        if (last_btn && !btn && storage_is_mounted()) {
            storage_info_t si = storage_info();
            status_display_set_storage(si.total_bytes, si.used_bytes, si.free_bytes);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        last_btn = btn;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
