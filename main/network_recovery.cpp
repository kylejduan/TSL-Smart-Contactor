#include "network.hpp"
#include "ota.hpp"
#include "esp_wifi.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
namespace app {
namespace {
void clear_dns(void*) {dns_clear_cache();}
}
esp_err_t recover_station_network() {
    if(!wifi_connected || provisioning || firmware_busy || critical_fault)
        return ESP_ERR_INVALID_STATE;
    // lwIP DNS state belongs to the TCP/IP thread. Never touch its tables from
    // the Fleet worker or replace DHCP's configured resolver addresses.
    if(tcpip_callback(clear_dns,nullptr)!=ERR_OK)return ESP_ERR_NO_MEM;
    const auto result=esp_wifi_disconnect();
    if(result==ESP_OK) {
        wifi_connected=false;
        utc_synced=false;
        // The existing reconnect task rejoins the saved profile. GOT_IP starts
        // fresh time synchronization; AUTO evidence and token NVS are untouched.
    }
    return result;
}
}
