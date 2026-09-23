#include "storage.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mbedtls/platform_util.h"
#include <cstdio>
#include <cstring>

namespace app {
namespace {
StaticSemaphore_t done_data;
SemaphoreHandle_t done=nullptr;
uint32_t scan_status=1;
void scan_done(void*,esp_event_base_t,int32_t,void* event) {
    scan_status=static_cast<wifi_event_sta_scan_done_t*>(event)->status;
    xSemaphoreGive(done);
}
}
// Only the serialized USB handler calls this, before a profile exists. No worker
// can start Wi-Fi until provisioning commits a profile and reboots the device.
void scan_unprovisioned_wifi(char* output,size_t capacity) {
    std::snprintf(output,capacity,"{\"ok\":false,\"error\":\"scan_unavailable\"}");
    static Profile profile;
    auto stored=load_profile(profile);
    mbedtls_platform_zeroize(&profile,sizeof profile);
    auto s=snapshot();
    if(stored!=ReadResult::Missing || s.ready || !s.config.disabled ||
       s.config.commissioned || s.decision.commanded || critical_fault || provisioning)return;
    static Ms next_scan=0;
    if(now_ms()<next_scan) {
        std::snprintf(output,capacity,"{\"ok\":false,\"error\":\"scan_rate_limited\"}");return;
    }
    next_scan=now_ms()+15000;
    static bool netif_initialized=false;
    if(!netif_initialized) {
        if(esp_netif_init()!=ESP_OK)return;
        netif_initialized=true;
    }
    if(esp_event_loop_create_default()!=ESP_OK)return;
    auto* netif=esp_netif_create_default_wifi_sta();
    bool initialized=false,started=false,registered=false;
    esp_event_handler_instance_t handler=nullptr;
    wifi_ap_record_t records[16]={};uint16_t count=16,total=0;
    bool ok=[&]() {
        if(!netif)return false;
        wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
        init.nvs_enable=0; // Scanning must not read or write saved Wi-Fi credentials.
        if(esp_wifi_init(&init)!=ESP_OK)return false;
        initialized=true;
        if(esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK)return false;
        done=xSemaphoreCreateBinaryStatic(&done_data);scan_status=1;
        if(!done || esp_event_handler_instance_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,scan_done,nullptr,&handler)!=ESP_OK)return false;
        registered=true;
        if(esp_wifi_start()!=ESP_OK)return false;
        started=true;
        wifi_scan_config_t scan={};scan.show_hidden=true;
        scan.scan_type=WIFI_SCAN_TYPE_PASSIVE;scan.scan_time.passive=200;
        if(esp_wifi_scan_start(&scan,false)!=ESP_OK)return false;
        if(xSemaphoreTake(done,pdMS_TO_TICKS(8000))!=pdTRUE || scan_status!=0)return false;
        if(esp_wifi_scan_get_ap_num(&total)!=ESP_OK)return false;
        if(!total) {count=0;return true;}
        return esp_wifi_scan_get_ap_records(&count,records)==ESP_OK;
    }();
    if(started) {
        if(esp_wifi_scan_stop()!=ESP_OK)fail("scan_cleanup");
        if(esp_wifi_clear_ap_list()!=ESP_OK)fail("scan_cleanup");
        if(esp_wifi_stop()!=ESP_OK)fail("scan_cleanup");
    }
    if(registered && esp_event_handler_instance_unregister(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,handler)!=ESP_OK)fail("scan_cleanup");
    if(initialized && esp_wifi_deinit()!=ESP_OK)fail("scan_cleanup");
    if(netif)esp_netif_destroy_default_wifi(netif);
    if(esp_event_loop_delete_default()!=ESP_OK)fail("scan_cleanup");
    if(!ok || critical_fault)return;
    size_t used=0;
    auto append=[&](const char* format,auto... args) {
        int n=std::snprintf(output+used,capacity-used,format,args...);
        if(n<0 || size_t(n)>=capacity-used)return false;
        used+=size_t(n);return true;
    };
    ok=append("{\"ok\":true,\"truncated\":%s,\"networks\":[",total>count?"true":"false");
    for(uint16_t i=0;ok && i<count;++i) {
        // SSIDs are untrusted bytes, not JSON or terminal text. Encode losslessly.
        char hex[65]={};constexpr char digits[]="0123456789abcdef";
        for(size_t j=0;j<32 && records[i].ssid[j];++j) {
            hex[2*j]=digits[records[i].ssid[j]>>4];hex[2*j+1]=digits[records[i].ssid[j]&15];
        }
        ok=append("%s{\"ssid_hex\":\"%s\",\"rssi\":%d,\"channel\":%u,\"authmode\":%u}",
                  i?",":"",hex,int(records[i].rssi),unsigned(records[i].primary),unsigned(records[i].authmode));
    }
    if(!ok || !append("]}"))std::snprintf(output,capacity,"{\"ok\":false,\"error\":\"scan_response_size\"}");
}
}
