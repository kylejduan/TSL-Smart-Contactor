#include "storage.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_random.h"
#include "esp_system.h"
#include "mbedtls/platform_util.h"
#include <cstring>
#include <cstdio>
namespace app {
namespace {
void send(const char* message) {
    usb_serial_jtag_write_bytes(message,std::strlen(message),pdMS_TO_TICKS(1000));
    usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(1000));
}
bool password(const Json& j,Profile& current) {
    char pw[129]={};
    bool ok=j.string(j.get(0,"password"),pw,sizeof pw) && verify_password(current,pw);
    mbedtls_platform_zeroize(pw,sizeof pw);return ok;
}
bool stop_network() {
    provisioning=true;auto epoch=inhibit();
    // A failed/restarted provisioning attempt must not revive prior AUTO settings.
    Profile previous;
    auto existing=load_profile(previous);mbedtls_platform_zeroize(&previous,sizeof previous);
    if(existing==ReadResult::Ok && !change_config(Change::Disabled,epoch))return false;
    Ms deadline=now_ms()+30000;
    while(network_busy && now_ms()<deadline)vTaskDelay(pdMS_TO_TICKS(50));
    return !network_busy;
}
bool provision(const Json& j) {
    if(!j.equal(j.get(0,"confirmation"),"REPLACE_CONFIG_USB_ONLY_MAINS_DISCONNECTED"))return false;
    if(!stop_network())return false;
    static Profile next;
    next=Profile{};
    int p=j.get(0,"profile"),c=j.get(p,"config");
    char pw[129]={},refresh[2049]={};
    bool ok=parse_config(j,c,next.config) &&
        j.string(j.get(p,"wifi_ssid"),next.ssid,sizeof next.ssid) &&
        j.string(j.get(p,"wifi_password"),next.wifi_password,sizeof next.wifi_password) &&
        j.string(j.get(p,"client_id"),next.client_id,sizeof next.client_id) &&
        j.string(j.get(p,"admin_password"),pw,sizeof pw) && std::strlen(pw)>=16 &&
        j.string(j.get(p,"refresh_token"),refresh,sizeof refresh) && refresh[0] &&
        j.string(j.get(p,"certificate"),next.certificate,sizeof next.certificate) &&
        j.string(j.get(p,"private_key"),next.private_key,sizeof next.private_key) &&
        j.string(j.get(p,"origin"),next.origin,sizeof next.origin);
    next.config.commissioned=false;next.config.disabled=true;next.config.dry_run=true;
    esp_fill_random(next.salt,sizeof next.salt);
    ok=ok && password_hash(pw,next.salt,next.password_hash) && valid_profile(next);
    if(ok) {
        uint8_t pending=1;
        ok=storage().write("provisioning",&pending,sizeof pending);
        TokenJournal journal(storage());
        ok=ok && journal.provision(refresh)==Error::None && save_profile(next);
        pending=0;ok=ok && storage().write("provisioning",&pending,sizeof pending);
        if(!ok)critical_fault=true;
    }
    mbedtls_platform_zeroize(pw,sizeof pw);mbedtls_platform_zeroize(refresh,sizeof refresh);
    mbedtls_platform_zeroize(&next,sizeof next);
    return ok;
}
void process(const char* input) {
    Json j;
    if(!j.parse(input)) {send("{\"ok\":false,\"error\":\"invalid_json\"}");return;}
    int op=j.get(0,"op");
    if(j.equal(op,"hello")) {
        send("{\"protocol\":1,\"board\":\"ESP32-S3-Relay-1CH\",\"firmware\":\"0.1.0\",\"secrets_echoed\":false}");return;
    }
    if(j.equal(op,"status")) {
        auto s=snapshot();char b[256]={};
        std::snprintf(b,sizeof b,"{\"ready\":%s,\"commissioned\":%s,\"disabled\":%s,\"dry_run\":%s,\"commanded_on\":%s,\"fault\":%s}",
            s.ready?"true":"false",s.config.commissioned?"true":"false",s.config.disabled?"true":"false",
            s.config.dry_run?"true":"false",s.decision.commanded?"true":"false",critical_fault?"true":"false");
        send(b);return;
    }
    if(j.equal(op,"provision")) {
        if(!provision(j)) {send("{\"ok\":false,\"error\":\"provision_failed_output_inhibited\"}");return;}
        send("{\"ok\":true,\"committed\":true,\"rebooting\":true}");
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));vTaskDelay(pdMS_TO_TICKS(500));esp_restart();
    }
    static Profile current;
    if(load_profile(current)!=ReadResult::Ok || !password(j,current)) {
        mbedtls_platform_zeroize(&current,sizeof current);
        send("{\"ok\":false,\"error\":\"authentication_required\"}");vTaskDelay(pdMS_TO_TICKS(2000));return;
    }
    bool ok=false;
    if(j.equal(op,"off")) {
        auto epoch=inhibit();current.config.disabled=true;ok=change_config(Change::Disabled,epoch);
    } else if(j.equal(op,"arm") && !critical_fault && !provisioning &&
              j.equal(j.get(0,"confirmation"),"USB_BENCH_POLARITY_AND_STARTUP_VERIFIED")) {
        auto epoch=inhibit();current.config.commissioned=true;current.config.disabled=true;
        // Arming does not energize and does not leave dry-run by itself.
        ok=change_config(Change::Arm,epoch);
    } else if(j.equal(op,"enable_output") && !critical_fault && !provisioning && current.config.commissioned &&
              j.equal(j.get(0,"confirmation"),"ALLOW_PHYSICAL_RELAY_AFTER_BENCH_CHECK")) {
        auto epoch=inhibit();current.config.dry_run=false;current.config.disabled=true;
        ok=change_config(Change::EnableOutput,epoch);
    } else if(j.equal(op,"auto") && !critical_fault && !provisioning) {
        auto epoch=inhibit();current.config.disabled=false;ok=change_config(Change::Auto,epoch);check_requested=true;
    } else if(j.equal(op,"timed_on") && !critical_fault && !provisioning) {
        int64_t seconds=3600;
        if(j.get(0,"seconds")>=0 && !j.integer(j.get(0,"seconds"),seconds))seconds=0;
        ok=seconds>0 && seconds<=28800 && timed(seconds,snapshot().generation);
    } else if(j.equal(op,"reboot")) {
        inhibit();send("{\"ok\":true,\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(500));esp_restart();
    }
    mbedtls_platform_zeroize(&current,sizeof current);
    send(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}");
}
}
void usb_task(void*) {
    usb_serial_jtag_driver_config_t c={.tx_buffer_size=2048,.rx_buffer_size=2048};
    if(usb_serial_jtag_driver_install(&c)!=ESP_OK) {critical_fault=true;vTaskDelete(nullptr);return;}
    static char line[16385];size_t length=0;bool discard=false;Ms started=0;
    while(true) {
        char ch;int n=usb_serial_jtag_read_bytes(&ch,1,pdMS_TO_TICKS(100));
        if(length && now_ms()-started>15000) {mbedtls_platform_zeroize(line,sizeof line);length=0;discard=true;}
        if(n!=1)continue;
        if(ch=='\n') {
            if(!discard && length) {line[length]=0;process(line);}
            else if(discard)send("{\"ok\":false,\"error\":\"request_size_or_timeout\"}");
            mbedtls_platform_zeroize(line,sizeof line);length=0;discard=false;continue;
        }
        if(discard || ch=='\r')continue;
        if(!length)started=now_ms();
        if(length+1>=sizeof line) {discard=true;continue;}
        line[length++]=ch; // never echo provisioning bytes
    }
}
}
