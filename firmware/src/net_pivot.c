#include "net_pivot.h"
#include "board_config.h"
#include "usb_net.h"
#include "wifi_ap.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "net_pivot";
static bool s_up = false;

bool net_pivot_is_up(void)
{
    return s_up;
}

const char *net_pivot_host_ip(void)
{
    return RNDISSTORAGE_HOST_IP_ADDR;
}

static esp_err_t pivot_apply(void)
{
    esp_netif_t *usb = usb_net_netif();
    esp_netif_t *ap = wifi_ap_netif();
    if (!usb || !ap) {
        return ESP_ERR_INVALID_STATE;
    }

    // SoftAP join/start can steal the default route; USB must stay upstream for NAPT.
    esp_err_t err = esp_netif_set_default_netif(usb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_default_netif(USB): %s", esp_err_to_name(err));
    }

    err = esp_netif_napt_enable(ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "napt_enable SoftAP failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (!s_up) {
        return;
    }
    if (id == WIFI_EVENT_AP_START || id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED) {
        esp_err_t err = pivot_apply();
        ESP_LOGI(TAG, "re-apply pivot after wifi event %ld → %s", (long)id, esp_err_to_name(err));
    }
}

esp_err_t net_pivot_start(void)
{
    if (s_up) {
        return pivot_apply();
    }

    esp_netif_t *usb = usb_net_netif();
    esp_netif_t *ap = wifi_ap_netif();
    ESP_RETURN_ON_FALSE(usb && ap, ESP_ERR_INVALID_STATE, TAG, "need USB + SoftAP netifs");

    // Ensure USB link is marked up for forwarding/ARP (DHCP-server netifs skip this otherwise).
    esp_netif_action_connected(usb, NULL, 0, NULL);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG,
                        "wifi event");

    ESP_RETURN_ON_ERROR(pivot_apply(), TAG, "pivot_apply");

    s_up = true;
    ESP_LOGI(TAG,
             "pivot UP — SoftAP → USB NAPT. From Wi-Fi client try: "
             "ping %s && curl http://%s:1234/  (host must listen + firewall allow)",
             RNDISSTORAGE_HOST_IP_ADDR, RNDISSTORAGE_HOST_IP_ADDR);
    return ESP_OK;
}
