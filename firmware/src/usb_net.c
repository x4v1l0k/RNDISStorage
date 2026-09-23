#include "usb_net.h"
#include "board_config.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb.h"

static const char *TAG = "usb_net";
static esp_netif_t *s_netif = NULL;

static void l2_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(100)) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_netif || !buffer || len == 0) {
        return ESP_OK;
    }
    void *copy = malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    esp_err_t err = esp_netif_receive(s_netif, copy, len, NULL);
    if (err != ESP_OK) {
        free(copy);
    }
    return err;
}

esp_err_t usb_net_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    vTaskDelay(pdMS_TO_TICKS(500));

    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install");

    // Hold disconnect so Windows drops Serial/JTAG and rebinds NCM (PID 0x4001).
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    tud_connect();

    tinyusb_net_config_t net_config = {
        .on_recv_callback = netif_recv_callback,
        .free_tx_buffer = NULL,
        .on_init_callback = NULL,
        .user_context = NULL,
        .mac_addr = {0x02, 0x00, 0x00, 0x71, 0x13, 0x01},
    };
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_config), TAG, "tinyusb_net_init");

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 203, 0, 113, 1);
    IP4_ADDR(&ip_info.gw, 203, 0, 113, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    static esp_netif_inherent_config_t base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = "USB_NCM",
        .if_desc = "usb-ncm",
        .route_prio = 10,
    };
    base_cfg.ip_info = &ip_info;

    static esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = netif_transmit,
        .driver_free_rx_buffer = l2_free,
    };

    static struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };

    const esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config,
    };

    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "esp_netif_new");

    uint8_t lwip_mac[6] = {0x02, 0x00, 0x00, 0x71, 0x13, 0x02};
    esp_netif_set_mac(s_netif, lwip_mac);
    esp_netif_action_start(s_netif, NULL, 0, NULL);

    ESP_LOGI(TAG, "USB NCM up — http://%s/", RNDISSTORAGE_IP_ADDR);
    return ESP_OK;
}
