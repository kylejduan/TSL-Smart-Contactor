#include "network.hpp"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <cstring>
namespace app {
static_assert(CONFIG_LWIP_DNS_MAX_SERVERS>=2,
    "IDF reserves the last DNS slot; DHCP needs a separate usable slot");
static void sync_callback(timeval*) {
    utc_last_sync_uptime_s=static_cast<uint32_t>(now_ms()/1000);
    utc_continuity=true;
    utc_synced=wifi_connected.load();
}
static void wifi_event(void*,esp_event_base_t base,int32_t id,void* data) {
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        utc_synced=false;
        wifi_connected=true;
        wifi_connect_error=0;
        // DNS and routing are available now. Also discard any old SNTP backoff
        // on reconnection; never accept an unsynchronized RTC as usable UTC.
        if(esp_netif_sntp_start()!=ESP_OK)fail("sntp_start");
    }
    if((base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) ||
       (base==IP_EVENT && id==IP_EVENT_STA_LOST_IP)) {
        utc_synced=false;
        wifi_connected=false;
        if(base==WIFI_EVENT && data)
            wifi_disconnect_reason=static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
    }
}
static void reconnect_task(void*) {
    uint32_t pause=2;
    while(true) {
        if(!wifi_connected && !provisioning) {
            ++wifi_connect_attempts;
            wifi_connect_error=static_cast<uint32_t>(esp_wifi_connect());
            pause=std::min<uint32_t>(60u,pause*2);
        } else pause=2;
        vTaskDelay(pdMS_TO_TICKS(pause*1000));
    }
}
void start_wifi(const Profile& p) {
    if(esp_netif_init()!=ESP_OK || esp_event_loop_create_default()!=ESP_OK) {fail("network_init_or_allocation");return;}
    auto* netif=esp_netif_create_default_wifi_sta();
    if(!netif || esp_netif_set_hostname(netif,"smart-contactor")!=ESP_OK) {fail("network_hostname");return;}
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&init)!=ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK ||
       esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr)!=ESP_OK ||
       esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr)!=ESP_OK ||
       esp_event_handler_register(IP_EVENT,IP_EVENT_STA_LOST_IP,wifi_event,nullptr)!=ESP_OK) {fail("network_init_or_allocation");return;}
    wifi_config_t cfg={};
    std::memcpy(cfg.sta.ssid,p.ssid,std::strlen(p.ssid));
    std::memcpy(cfg.sta.password,p.wifi_password,std::strlen(p.wifi_password));
    cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable=true;
    // Accept both SAE derivation methods for WPA3-Personal APs, as in the
    // pinned ESP-IDF station example. WPA2 remains available where configured.
    cfg.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
    esp_sntp_config_t time_config=ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("time.cloudflare.com","pool.ntp.org"));
    time_config.start=false;
    time_config.sync_cb=sync_callback;
    if(esp_netif_sntp_init(&time_config)!=ESP_OK) {fail("sntp_init");return;}
    if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK || esp_wifi_set_config(WIFI_IF_STA,&cfg)!=ESP_OK ||
       esp_wifi_start()!=ESP_OK)fail("network_init_or_allocation");
    mbedtls_platform_zeroize(&cfg,sizeof cfg);
    if(xTaskCreate(reconnect_task,"wifi_retry",3072,nullptr,2,nullptr)!=pdPASS)fail("network_init_or_allocation");
}
}
