#include "storage.hpp"
#include "esp_https_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "mbedtls/platform_util.h"
#include <cstdio>
#include <cstring>
#include <ctime>
namespace app {
namespace {
Profile* profile=nullptr;
std::atomic<uint32_t> auth_mode{1};
Session session;
char csrf[65]={};
// One bounded verification job; sessions are still owned only by the HTTP task.
struct LoginJob {
    httpd_req_t* request=nullptr;
    char password[129]={};
    char material[65]={};
    std::atomic<bool> verified{false};
    std::atomic<bool> storage_fault{false};
};
LoginJob login_job;
std::atomic<bool> login_busy{false};
extern "C" const unsigned char html_start[] asm("_binary_index_html_start");
extern "C" const unsigned char html_end[] asm("_binary_index_html_end");
extern "C" const unsigned char css_start[] asm("_binary_style_css_start");
extern "C" const unsigned char css_end[] asm("_binary_style_css_end");
extern "C" const unsigned char js_start[] asm("_binary_app_js_start");
extern "C" const unsigned char js_end[] asm("_binary_app_js_end");
void headers(httpd_req_t* r) {
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    httpd_resp_set_hdr(r,"Referrer-Policy","no-referrer");
}
esp_err_t reply(httpd_req_t* r,const char* body,const char* status="200 OK") {
    headers(r);httpd_resp_set_status(r,status);httpd_resp_set_type(r,"application/json");
    return httpd_resp_send(r,body,HTTPD_RESP_USE_STRLEN);
}
bool same_origin(httpd_req_t* r) {
    char origin[193]={};
    return httpd_req_get_hdr_value_str(r,"Origin",origin,sizeof origin)==ESP_OK &&
        constant_equal(origin,profile->origin);
}
bool cookie(httpd_req_t* r,char* out) {
    char header[192]={};
    if(httpd_req_get_hdr_value_str(r,"Cookie",header,sizeof header)!=ESP_OK)return false;
    std::string_view s(header);unsigned found=0;
    while(!s.empty()) {
        auto end=s.find(';');auto part=s.substr(0,end);
        while(!part.empty() && part.front()==' ')part.remove_prefix(1);
        constexpr std::string_view prefix="__Host-id=";
        if(part.substr(0,prefix.size())==prefix) {
            auto value=part.substr(prefix.size());if(value.size()!=64 || ++found!=1)return false;
            std::memcpy(out,value.data(),64);out[64]=0;
        }
        if(end==s.npos)break;
        s.remove_prefix(end+1);
    }
    return found==1;
}
bool auth(httpd_req_t* r,bool change) {
    char id[65]={},supplied[65]={};
    if(!cookie(r,id))return false;
    if(!change)return session.authorized(id,now_ms());
    return same_origin(r) && httpd_req_get_hdr_value_str(r,"X-CSRF-Token",supplied,sizeof supplied)==ESP_OK &&
        session.change_allowed(id,supplied,now_ms());
}
bool body(httpd_req_t* r,char* buffer,size_t capacity) {
    char type[64]={};
    if(r->content_len<=0 || size_t(r->content_len)>=capacity ||
       httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof type)!=ESP_OK ||
       std::string_view(type)!="application/json")return false;
    size_t n=0;Ms deadline=now_ms()+3000;
    while(n<size_t(r->content_len)) {
        if(now_ms()>deadline)return false;
        int got=httpd_req_recv(r,buffer+n,r->content_len-n);
        if(got<=0)return false;
        n+=got;
    }
    buffer[n]=0;return true;
}
void random_hex(char* out) {
    uint8_t bytes[32];esp_fill_random(bytes,sizeof bytes);
    for(size_t i=0;i<sizeof bytes;++i)std::snprintf(out+i*2,3,"%02x",bytes[i]);
    mbedtls_platform_zeroize(bytes,sizeof bytes);
}
esp_err_t root(httpd_req_t* r) {
    headers(r);httpd_resp_set_type(r,"text/html; charset=utf-8");
    return httpd_resp_send(r,reinterpret_cast<const char*>(html_start),html_end-html_start-1);
}
esp_err_t styles(httpd_req_t* r) {
    headers(r);httpd_resp_set_type(r,"text/css; charset=utf-8");
    return httpd_resp_send(r,reinterpret_cast<const char*>(css_start),css_end-css_start-1);
}
esp_err_t script(httpd_req_t* r) {
    headers(r);httpd_resp_set_type(r,"text/javascript; charset=utf-8");
    return httpd_resp_send(r,reinterpret_cast<const char*>(js_start),js_end-js_start-1);
}
esp_err_t events(httpd_req_t* r) {
    if(!auth(r,false))return reply(r,"{\"error\":\"authentication_required\"}","401 Unauthorized");
    const auto s=snapshot();char output[2048]={};size_t used=0;
    int n=std::snprintf(output,sizeof output,"{\"events\":[");
    if(n<0)return reply(r,"{\"error\":\"status_unavailable\"}","503 Service Unavailable");
    used=n;
    for(size_t i=0;i<s.events.size();++i) {
        const auto& e=*s.events.newest(i);
        n=std::snprintf(output+used,sizeof output-used,
            "%s{\"uptime_s\":%lld,\"reason\":\"%s\",\"commanded_on\":%s}",
            i?",":"",static_cast<long long>(e.at/1000),reason_name(e.reason),e.commanded?"true":"false");
        if(n<0 || size_t(n)>=sizeof output-used)
            return reply(r,"{\"error\":\"status_unavailable\"}","503 Service Unavailable");
        used+=n;
    }
    if(used+3>=sizeof output)return reply(r,"{\"error\":\"status_unavailable\"}","503 Service Unavailable");
    std::strcpy(output+used,"]}");return reply(r,output);
}
esp_err_t login_info(httpd_req_t* r) {
    char result[96]={};char salt[33]={};
    for(size_t i=0;i<16;++i)std::snprintf(salt+i*2,3,"%02x",profile->salt[i]);
    std::snprintf(result,sizeof result,"{\"version\":%lu,\"salt\":\"%s\",\"iterations\":100000}",
                  static_cast<unsigned long>(auth_mode.load()),salt);
    return reply(r,result);
}
void finish_login(void*) {
    auto* r=login_job.request;
    if(login_job.storage_fault)reply(r,"{\"error\":\"login_unavailable\"}","503 Service Unavailable");
    else if(!login_job.verified) {
        session.failed_login(now_ms());reply(r,"{\"error\":\"login_failed\"}","401 Unauthorized");
    } else {
        char id[65]={},header[192]={};random_hex(id);random_hex(csrf);
        session.establish(id,csrf,now_ms());
        std::snprintf(header,sizeof header,"__Host-id=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=900",id);
        httpd_resp_set_hdr(r,"Set-Cookie",header);
        if(reply(r,"{\"ok\":true}")!=ESP_OK)session.clear();
        mbedtls_platform_zeroize(id,sizeof id);mbedtls_platform_zeroize(header,sizeof header);
    }
    if(httpd_req_async_handler_complete(r)!=ESP_OK)fail("login_complete");
    login_job.request=nullptr;login_busy=false;
}
void verify_login(void*) {
    login_job.verified=login_job.material[0]
        ? verify_derived(*profile,login_job.material)
        : verify_password(*profile,login_job.password);
    if(login_job.verified && profile->version==1) {
        if(migrate_password(*profile))auth_mode.store(2);
        else {login_job.storage_fault=true;login_job.verified=false;fail("password_migration_write");}
    }
    mbedtls_platform_zeroize(login_job.password,sizeof login_job.password);
    mbedtls_platform_zeroize(login_job.material,sizeof login_job.material);
    // Publish the result in HTTPD context so no session or cookie state is raced.
    if(httpd_queue_work(login_job.request->handle,finish_login,nullptr)!=ESP_OK) {
        fail("login_result_queue");
        httpd_req_async_handler_complete(login_job.request);
        login_job.request=nullptr;login_busy=false;
    }
    vTaskDelete(nullptr);
}
esp_err_t login(httpd_req_t* r) {
    if(!same_origin(r))return reply(r,"{\"error\":\"origin\"}","403 Forbidden");
    if(login_busy || !session.login_allowed(now_ms()))return reply(r,"{\"error\":\"login_throttled\"}","429 Too Many Requests");
    char input[512]={};Json j;
    bool ok=body(r,input,sizeof input) && j.parse(input);
    if(ok) {
        bool raw=j.string(j.get(0,"password"),login_job.password,sizeof login_job.password);
        bool derived=j.string(j.get(0,"material"),login_job.material,sizeof login_job.material);
        ok=(raw!=derived) && (raw || derived);
    }
    mbedtls_platform_zeroize(input,sizeof input);
    if(!ok) {
        mbedtls_platform_zeroize(login_job.password,sizeof login_job.password);
        mbedtls_platform_zeroize(login_job.material,sizeof login_job.material);
        session.failed_login(now_ms());return reply(r,"{\"error\":\"login_failed\"}","401 Unauthorized");
    }
    if(httpd_req_async_handler_begin(r,&login_job.request)!=ESP_OK) {
        mbedtls_platform_zeroize(login_job.password,sizeof login_job.password);
        mbedtls_platform_zeroize(login_job.material,sizeof login_job.material);
        fail("login_request_allocation");return reply(r,"{\"error\":\"login_unavailable\"}","503 Service Unavailable");
    }
    login_busy=true;
    login_job.verified=false;login_job.storage_fault=false;
    if(xTaskCreate(verify_login,"password_check",6144,nullptr,2,nullptr)!=pdPASS) {
        mbedtls_platform_zeroize(login_job.password,sizeof login_job.password);
        mbedtls_platform_zeroize(login_job.material,sizeof login_job.material);
        fail("login_task");
        reply(login_job.request,"{\"error\":\"login_unavailable\"}","503 Service Unavailable");
        httpd_req_async_handler_complete(login_job.request);
        login_job.request=nullptr;login_busy=false;
    }
    return ESP_OK;
}
esp_err_t status(httpd_req_t* r) {
    if(!auth(r,false))return reply(r,"{\"error\":\"authentication_required\"}","401 Unauthorized");
    char output[4096]={};
    if(!status_json(output,sizeof output))return reply(r,"{\"error\":\"status_unavailable\"}","503 Service Unavailable");
    // CSRF lives only in page memory and this authenticated no-store response.
    httpd_resp_set_hdr(r,"X-CSRF-Token",csrf);
    return reply(r,output);
}
esp_err_t action(httpd_req_t* r) {
    if(!auth(r,true))return reply(r,"{\"error\":\"authentication_or_csrf\"}","403 Forbidden");
    char input[2048]={};Json j;
    if(!body(r,input,sizeof input) || !j.parse(input))return reply(r,"{\"error\":\"invalid_request\"}","400 Bad Request");
    auto s=snapshot();int op=j.get(0,"action");
    if(j.equal(op,"logout")) {
        session.clear();
        httpd_resp_set_hdr(r,"Set-Cookie","__Host-id=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    } else if(j.equal(op,"off")) {
        auto epoch=inhibit();s.config.disabled=true;
        if(!change_config(Change::Disabled,epoch))return reply(r,"{\"error\":\"off_inhibited_persistence_failed\"}","503 Service Unavailable");
    } else if(provisioning || critical_fault || !s.ready) {
        return reply(r,"{\"error\":\"usb_recovery_required\"}","409 Conflict");
    } else if(j.equal(op,"auto")) {
        auto epoch=inhibit();s.config.disabled=false;
        if(!change_config(Change::Auto,epoch))return reply(r,"{\"error\":\"persistence_failed\"}","503 Service Unavailable");
        check_requested=true;
    } else if(j.equal(op,"timed_on")) {
        int64_t seconds=3600;
        if(j.get(0,"seconds")>=0 && !j.integer(j.get(0,"seconds"),seconds))seconds=0;
        if(seconds<=0 || seconds>28800 || !timed(seconds,s.generation))
            return reply(r,"{\"error\":\"timed_on_rejected\"}","409 Conflict");
    } else if(j.equal(op,"check_now")) {
        static Ms next_manual=0;
        if(s.config.disabled)return reply(r,"{\"error\":\"check_requires_auto\"}","409 Conflict");
        if(!wifi_connected || !s.utc_ok)return reply(r,"{\"error\":\"connection_not_ready\"}","409 Conflict");
        if(now_ms()<next_manual || network_busy || (s.error!=Error::None && now_ms()<s.next_poll))return reply(r,"{\"error\":\"check_throttled\"}","429 Too Many Requests");
        next_manual=now_ms()+600000;check_requested=true;
    } else if(j.equal(op,"settings")) {
        Config c=s.config;
        if(!parse_config(j,j.get(0,"settings"),c))return reply(r,"{\"error\":\"invalid_settings\"}","400 Bad Request");
        // Arming and enabling physical output are USB-only. Web may enter dry-run.
        if(s.config.dry_run && !c.dry_run)return reply(r,"{\"error\":\"usb_commissioning_required\"}","409 Conflict");
        auto epoch=inhibit();
        if(!change_config(Change::Settings,epoch,&c))return reply(r,"{\"error\":\"persistence_failed\"}","503 Service Unavailable");
    } else return reply(r,"{\"error\":\"unknown_action\"}","400 Bad Request");
    return reply(r,"{\"ok\":true}");
}
}
size_t status_json(char* out,size_t capacity) {
    auto s=snapshot();auto d=s.decision;
    wifi_ap_record_t ap{};int rssi=wifi_connected && esp_wifi_sta_get_ap_info(&ap)==ESP_OK ? ap.rssi : 0;
    auto remaining=s.polling_paused || s.config.disabled ? -1 : std::max<Ms>(0,s.next_poll-now_ms())/1000;
    long long age=d.last_source_s && s.utc_ok ? time(nullptr)-d.last_source_s : -1;
    int n=std::snprintf(out,capacity,
        "{\"mode\":\"%s\",\"commissioned\":%s,\"dry_run\":%s,\"auto_home\":%s,"
        "\"desired_on\":%s,\"gpio_command\":\"%s commanded\",\"reason\":\"%s\","
        "\"lease_s\":%lld,\"override_s\":%lld,\"vehicle\":\"%s\",\"location_age_s\":%lld,"
        "\"distance_m\":%.1f,\"last_success_uptime_s\":%lld,\"next_poll_s\":%lld,"
        "\"wifi_connected\":%s,\"rssi_dbm\":%d,\"uptime_s\":%lld,\"utc_ready\":%s,"
        "\"error\":\"%s\",\"reauthorization_needed\":%s,\"poll_busy\":%s,"
        "\"fleet_endpoint\":\"%s\",\"fleet_http_status\":%d,\"fleet_detail\":\"%s\",\"gps_source_value\":%.17g,\"gps_source_text\":\"%s\",\"fleet_txid\":\"%s\",\"fleet_date\":\"%s\",\"fleet_received_utc_s\":%lld,\"report_timestamp_text\":\"%s\",\"api_version\":%lld,\"reported_distance_m\":%.1f,"
        "\"attempts_this_boot\":[%lu,%lu,%lu],\"reserved_today\":[%lu,%lu,%lu],\"reserved_month\":[%lu,%lu,%lu],\"location_monthly_cap\":%lu,\"estimated_location_usd\":%.3f,"
        "\"ready\":%s,\"fault\":%s,\"fault_source\":\"%s\",\"control_max_gap_ms\":%u,\"internal_heap_free\":%u,"
        "\"firmware_version\":\"%s\",\"sdk_version\":\"%s\",\"session_left_s\":%lld,"
        "\"settings\":{\"vin\":\"%s\",\"home_lat\":%.7f,\"home_lon\":%.7f,\"enable_m\":%lu,\"disable_m\":%lu,"
        "\"max_age_s\":%lu,\"future_s\":%lu,\"lease_s\":%lu,\"sleep_s\":%lu,\"poll_s\":%lu,\"dwell_s\":%lu,"
        "\"daily_cap\":%lu,\"monthly_cap\":%lu,\"region\":\"%s\",\"dry_run\":%s}}",
        s.config.disabled?"DISABLED":(d.timed?"TIMED_ON":"AUTO"),s.config.commissioned?"true":"false",s.config.dry_run?"true":"false",
        d.auto_home?"true":"false",d.desired?"true":"false",d.commanded?"ON":"OFF",reason_name(d.reason),
        d.lease_left/1000,d.override_left/1000,vehicle_name(s.vehicle),age,d.distance,s.last_poll/1000,remaining,
        wifi_connected?"true":"false",rssi,now_ms()/1000,s.utc_ok?"true":"false",error_name(s.error),
        s.error==Error::Reauthorize || s.error==Error::Authentication || s.error==Error::Permission ?"true":"false",network_busy?"true":"false",
        s.fleet.endpoint,s.fleet.http_status,s.fleet.detail,s.fleet.gps_source_value,s.fleet.gps_source_text,
        s.fleet.transaction_id,s.fleet.response_date,static_cast<long long>(s.fleet.received_utc_s),
        s.fleet.vehicle_metadata.report_timestamp_text,static_cast<long long>(s.fleet.vehicle_metadata.api_version),s.fleet.reported_distance_m,
        (unsigned long)s.fleet.attempts_this_boot[0],(unsigned long)s.fleet.attempts_this_boot[1],(unsigned long)s.fleet.attempts_this_boot[2],
        (unsigned long)s.budget.daily[0],(unsigned long)s.budget.daily[1],(unsigned long)s.budget.daily[2],
        (unsigned long)s.budget.monthly[0],(unsigned long)s.budget.monthly[1],(unsigned long)s.budget.monthly[2],
        (unsigned long)kMonthlyDataRequestCap,s.budget.monthly[1]*kDataRequestUsd,
        s.ready?"true":"false",critical_fault?"true":"false",fault_source.load()?fault_source.load():"none",
        unsigned(control_max_gap_ms.load()),unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        esp_app_get_description()->version,esp_app_get_description()->idf_ver,static_cast<long long>(session.remaining(now_ms())/1000),
        s.config.vin,s.config.home_lat,s.config.home_lon,(unsigned long)s.config.enable_m,(unsigned long)s.config.disable_m,
        (unsigned long)s.config.max_age_s,(unsigned long)s.config.future_s,(unsigned long)s.config.lease_s,(unsigned long)s.config.sleep_s,
        (unsigned long)s.config.poll_s,(unsigned long)s.config.dwell_s,(unsigned long)s.config.daily_cap,(unsigned long)s.config.monthly_cap,
        s.config.region?"EU":"NA",s.config.dry_run?"true":"false");
    return n>0 && size_t(n)<capacity ? size_t(n) : 0;
}
bool start_management(Profile& p) {
    profile=&p;
    auth_mode.store(p.version);
    httpd_ssl_config_t cfg=HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.stack_size=24576;cfg.httpd.max_open_sockets=3;cfg.httpd.max_uri_handlers=8;
    cfg.httpd.recv_wait_timeout=2;cfg.httpd.send_wait_timeout=2;cfg.httpd.lru_purge_enable=true;
    cfg.servercert=reinterpret_cast<const uint8_t*>(p.certificate);cfg.servercert_len=std::strlen(p.certificate)+1;
    cfg.prvtkey_pem=reinterpret_cast<const uint8_t*>(p.private_key);cfg.prvtkey_len=std::strlen(p.private_key)+1;
    httpd_handle_t server=nullptr;
    if(httpd_ssl_start(&server,&cfg)!=ESP_OK)return false;
    struct Route {const char* path;httpd_method_t method;esp_err_t (*handler)(httpd_req_t*);};
    const Route routes[]={{"/",HTTP_GET,root},{"/style.css",HTTP_GET,styles},{"/app.js",HTTP_GET,script},
        {"/api/events",HTTP_GET,events},{"/api/login-info",HTTP_GET,login_info},
        {"/api/login",HTTP_POST,login},
        {"/api/status",HTTP_GET,status},{"/api/action",HTTP_POST,action}};
    for(const auto& r:routes) {
        httpd_uri_t route={};route.uri=r.path;route.method=r.method;route.handler=r.handler;
        if(httpd_register_uri_handler(server,&route)!=ESP_OK) {httpd_ssl_stop(server);return false;}
    }
    return true;
}
}
