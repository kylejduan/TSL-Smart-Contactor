#include "network.hpp"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "http_decoder.hpp"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
namespace app {
static_assert(CONFIG_LWIP_DNS_MAX_SERVERS>=2,
    "IDF reserves the last DNS slot; DHCP needs a separate usable slot");
static void sync_callback(timeval*) {utc_synced=true;}
static void wifi_event(void*,esp_event_base_t base,int32_t id,void*) {
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        wifi_connected=true;
        // DNS and routing are available now. Also discard any old SNTP backoff
        // on reconnection; never accept an unsynchronized RTC as usable UTC.
        if(esp_netif_sntp_start()!=ESP_OK)fail("sntp_start");
    }
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED)wifi_connected=false;
}
static void reconnect_task(void*) {
    uint32_t pause=2;
    while(true) {
        if(!wifi_connected && !provisioning) {
            esp_wifi_connect();pause=std::min<uint32_t>(60u,pause*2);
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
       esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr)!=ESP_OK) {fail("network_init_or_allocation");return;}
    wifi_config_t cfg={};
    std::memcpy(cfg.sta.ssid,p.ssid,std::strlen(p.ssid));
    std::memcpy(cfg.sta.password,p.wifi_password,std::strlen(p.wifi_password));
    cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable=true;
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
void request(Endpoint endpoint,const Config& cfg,const char* access,const char* form,HttpResult& r) {
    if(!snapshot().utc_ok) {r.error=Error::Clock;return;}
    if(!wifi_connected) {r.error=Error::Wifi;return;}
    const char* host=endpoint==Endpoint::Refresh ? "fleet-auth.prd.vn.cloud.tesla.com" :
        (cfg.region ? "fleet-api.prd.eu.vn.cloud.tesla.com" : "fleet-api.prd.na.vn.cloud.tesla.com");
    char path[128]={},header[4608]={};
    bool post=endpoint==Endpoint::Refresh;
    if(post)std::strcpy(path,"/oauth2/v3/token");
    else std::snprintf(path,sizeof path,"/api/1/vehicles/%s%s",cfg.vin,
        endpoint==Endpoint::Location ? "/vehicle_data?endpoints=location_data" : "");
    int n=post ? std::snprintf(header,sizeof header,
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",
        path,host,unsigned(std::strlen(form))) : std::snprintf(header,sizeof header,
        "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",path,host,access);
    if(n<=0 || size_t(n)>=sizeof header) {r.error=Error::Malformed;return;}
    esp_tls_cfg_t config={};config.crt_bundle_attach=esp_crt_bundle_attach;
    config.non_block=true;config.timeout_ms=10000;
    auto* tls=esp_tls_init();
    if(!tls) {fail("network_init_or_allocation");r.error=Error::Storage;return;}
    const Ms deadline=now_ms()+20000,connect_deadline=now_ms()+10000;
    int connected=0;
    do {
        connected=esp_tls_conn_new_async(host,std::strlen(host),443,&config,tls);
        if(connected!=0)break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while(now_ms()<connect_deadline);
    if(connected!=1)r.error=connected<0 ? Error::Transport : Error::Timeout;
    auto write=[&](const char* p,size_t length) {
        size_t sent=0;Ms progress=now_ms();
        while(sent<length) {
            if(now_ms()>=deadline || now_ms()-progress>=5000) {r.error=Error::Timeout;return false;}
            int result=esp_tls_conn_write(tls,p+sent,length-sent);
            if(result>0) {sent+=result;progress=now_ms();}
            else if(result!=ESP_TLS_ERR_SSL_WANT_READ && result!=ESP_TLS_ERR_SSL_WANT_WRITE) {
                r.error=Error::Transport;return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return true;
    };
    HttpDecoder decoder(r.body);
    if(r.error==Error::None && write(header,n) && (!post || write(form,std::strlen(form)))) {
        char data[1024];Ms progress=now_ms();
        while(!decoder.done()) {
            if(now_ms()>=deadline || now_ms()-progress>=5000) {r.error=Error::Timeout;break;}
            int got=esp_tls_conn_read(tls,data,sizeof data);
            if(got>0) {
                progress=now_ms();
                if(!decoder.feed(data,got)) {r.error=decoder.error();break;}
            } else if(got==0) {if(!decoder.eof())r.error=decoder.error();break;}
            else if(got!=ESP_TLS_ERR_SSL_WANT_READ && got!=ESP_TLS_ERR_SSL_WANT_WRITE) {
                r.error=Error::Transport;break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        mbedtls_platform_zeroize(data,sizeof data);
    }
    r.status=decoder.status();
    r.received_utc_s=time(nullptr);
    std::strcpy(r.transaction_id,decoder.transaction_id());
    std::strcpy(r.response_date,decoder.response_date());
    if(r.error==Error::None) {
        if(!decoder.done())r.error=Error::Malformed;
        else r.error=http_error(r.status);
    }
    const char* retry=decoder.retry_after();
    if(*retry) {
        char* end=nullptr;unsigned long delay=std::strtoul(retry,&end,10);
        if(end && !*end)r.retry_s=std::min(delay,86400ul);
        else {
            tm t{};
            if(strptime(retry,"%a, %d %b %Y %H:%M:%S GMT",&t))
                r.retry_s=std::clamp<int64_t>(mktime(&t)-time(nullptr),0,86400);
            else r.retry_s=3600;
        }
    }
    esp_tls_conn_destroy(tls);
    mbedtls_platform_zeroize(header,sizeof header);
}
}
