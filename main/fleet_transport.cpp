#include "network.hpp"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "http_decoder.hpp"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
namespace app {
void request(Endpoint endpoint,const Config& cfg,const char* access,const char* form,HttpResult& r) {
    const Ms started=now_ms();
    r.transport={};
    r.request_may_have_been_sent=false;
    if(!snapshot().utc_ok) {r.transport.phase="clock";r.error=Error::Clock;return;}
    if(!wifi_connected) {r.transport.phase="wifi";r.error=Error::Wifi;return;}
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
    if(n<=0 || size_t(n)>=sizeof header) {
        mbedtls_platform_zeroize(header,sizeof header);
        r.transport.phase="headers";r.error=Error::Malformed;return;
    }
    esp_tls_cfg_t config={};config.crt_bundle_attach=esp_crt_bundle_attach;
    config.non_block=true;config.timeout_ms=10000;
    r.transport.phase="allocate";
    auto* tls=esp_tls_init();
    if(!tls) {
        mbedtls_platform_zeroize(header,sizeof header);
        r.error=Error::Transport;return;
    }
    const Ms deadline=now_ms()+20000,connect_deadline=now_ms()+10000;
    r.transport.phase="connect";
    int connected=0;
    do {
        connected=esp_tls_conn_new_async(host,std::strlen(host),443,&config,tls);
        if(connected!=0)break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while(now_ms()<connect_deadline);
    // The SDK's first async call still performs synchronous DNS resolution and
    // a bounded socket select. Reject late completion before sending HTTP data.
    if(now_ms()>=connect_deadline)r.error=Error::Timeout;
    else if(connected!=1)r.error=connected<0 ? Error::Transport : Error::Timeout;
    r.transport.connect_ms=uint32_t(std::min<Ms>(UINT32_MAX,now_ms()-started));
    auto write=[&](const char* p,size_t length,const char* phase) {
        r.transport.phase=phase;
        size_t sent=0;Ms progress=now_ms();
        while(sent<length) {
            if(now_ms()>=deadline || now_ms()-progress>=5000) {r.error=Error::Timeout;return false;}
            // Set before the call: even a failed/partial write remains ambiguous.
            r.request_may_have_been_sent=true;
            int result=esp_tls_conn_write(tls,p+sent,length-sent);
            r.transport.last_io=result;
            if(result>0) {sent+=result;r.transport.bytes_written+=uint32_t(result);progress=now_ms();}
            else if(result!=ESP_TLS_ERR_SSL_WANT_READ && result!=ESP_TLS_ERR_SSL_WANT_WRITE) {
                r.transport.system_error=errno;
                r.error=Error::Transport;return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return true;
    };
    HttpDecoder decoder(r.body);
    if(r.error==Error::None && write(header,n,"write_headers") && (!post || write(form,std::strlen(form),"write_body"))) {
        r.transport.phase="read_response";
        char data[1024];Ms progress=now_ms();
        while(!decoder.done()) {
            if(now_ms()>=deadline || now_ms()-progress>=5000) {r.error=Error::Timeout;break;}
            int got=esp_tls_conn_read(tls,data,sizeof data);
            r.transport.last_io=got;
            if(got>0) {
                r.transport.bytes_read+=uint32_t(got);
                progress=now_ms();
                if(!decoder.feed(data,got)) {r.error=decoder.error();break;}
            } else if(got==0) {if(!decoder.eof())r.error=decoder.error();break;}
            else if(got!=ESP_TLS_ERR_SSL_WANT_READ && got!=ESP_TLS_ERR_SSL_WANT_WRITE) {
                r.transport.system_error=errno;
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
    r.retry_s=retry_after_seconds(decoder.retry_after(),time(nullptr));
    esp_tls_conn_state_t state{};
    if(esp_tls_get_conn_state(tls,&state)==ESP_OK)r.transport.connect_state=int(state);
    esp_tls_error_handle_t handle=nullptr;
    if(esp_tls_get_error_handle(tls,&handle)==ESP_OK && handle) {
        int system_error=0;
        if(esp_tls_get_and_clear_error_type(handle,ESP_TLS_ERR_TYPE_SYSTEM,&system_error)==ESP_OK && system_error)
            r.transport.system_error=system_error;
        r.transport.tls_error=esp_tls_get_and_clear_last_error(handle,&r.transport.mbedtls_error,&r.transport.verify_flags);
    }
    r.transport.request_may_have_been_sent=r.request_may_have_been_sent;
    r.transport.elapsed_ms=uint32_t(std::min<Ms>(UINT32_MAX,now_ms()-started));
    if(r.error==Error::None)r.transport.phase="complete";
    esp_tls_conn_destroy(tls);
    mbedtls_platform_zeroize(header,sizeof header);
}
}
