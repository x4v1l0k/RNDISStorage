#include "wifi_ap.h"
#include "board_config.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_ap";
static bool s_up = false;
static esp_netif_t *s_ap_netif = NULL;

bool wifi_ap_is_up(void)
{
    return s_up;
}

const char *wifi_ap_ssid(void)
{
    return RNDISSTORAGE_WIFI_SSID;
}

const char *wifi_ap_ip(void)
{
    return RNDISSTORAGE_WIFI_IP_ADDR;
}

static esp_err_t ensure_netif_stack(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_RETURN_ON_ERROR(ret, TAG, "nvs");
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "netif init");
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "event loop");
    }
    return ESP_OK;
}

esp_err_t wifi_ap_start(void)
{
    if (s_up) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_netif_stack(), TAG, "netif stack");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "ap netif");

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    (void)RNDISSTORAGE_WIFI_IP_ADDR;

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(s_ap_netif), TAG, "dhcps stop");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG, "set ip");

    // Force DHCP to advertise a default gateway — required so SoftAP clients
    // route 203.0.113.0/24 via the dongle instead of cellular/other NIC.
    uint8_t offer_router = 1;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                               &offer_router, sizeof(offer_router)),
                        TAG, "dhcps router");

    // Captive-portal URI helps Android/iOS treat this Wi-Fi as a usable network
    // and keep SoftAP as the default route (disable mobile data when testing).
    char captive[] = "http://192.168.4.1/";
    esp_err_t cap_err =
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captive, strlen(captive) + 1);
    if (cap_err != ESP_OK) {
        ESP_LOGW(TAG, "captive portal DHCP option: %s", esp_err_to_name(cap_err));
    }

    uint8_t offer_dns = 1;
    if (esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                               sizeof(offer_dns)) == ESP_OK) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
        esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    }

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "dhcps start");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode");

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, RNDISSTORAGE_WIFI_SSID, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, RNDISSTORAGE_WIFI_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len = (uint8_t)strlen(RNDISSTORAGE_WIFI_SSID);
    ap.ap.channel = RNDISSTORAGE_WIFI_CHANNEL;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.ssid_hidden = 0;
    if (strlen(RNDISSTORAGE_WIFI_PASS) < 8) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    s_up = true;
    ESP_LOGI(TAG, "SoftAP up SSID=%s IP=http://%s/ (DHCP advertises GW+DNS+captive)", RNDISSTORAGE_WIFI_SSID,
             RNDISSTORAGE_WIFI_IP_ADDR);
    return ESP_OK;
}

esp_netif_t *wifi_ap_netif(void)
{
    return s_ap_netif;
}
