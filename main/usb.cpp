#include "storage.hpp"
#include "network.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "mbedtls/platform_util.h"
#include <cstring>
#include <cstdio>
#include <ctime>
namespace app {
namespace {
const char* provision_stage="idle";
const char* read_name(ReadResult r) {
    return r==ReadResult::Ok ? "present" : r==ReadResult::Missing ? "missing" : "error";
}
void diagnostics() {
    // Metadata only. Never emit credentials, profile fields or token contents.
    static Profile p;
    auto raw=storage().read("profile",&p,sizeof p);
    bool valid=raw==ReadResult::Ok && intact(p) && valid_config(p.config);
    mbedtls_platform_zeroize(&p,sizeof p);
    uint8_t pending=0;
    auto marker=storage().read("provisioning",&pending,sizeof pending);
    TokenJournal journal(storage());auto token=journal.load();
    esp_netif_ip_info_t ip={};
    esp_netif_dns_info_t dns={};
    auto* netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif) {
        esp_netif_get_ip_info(netif,&ip);
        esp_netif_get_dns_info(netif,ESP_NETIF_DNS_MAIN,&dns);
    }
    auto cause=fault_source.load();
    uint8_t mac[6]={};esp_read_mac(mac,ESP_MAC_WIFI_STA);
    auto state=snapshot();
    char b[1792]={};
    std::snprintf(b,sizeof b,
        "{\"profile_record\":\"%s\",\"profile_integrity\":%s,\"provision_marker\":\"%s\",\"provision_pending\":%s,"
        "\"token_state\":\"%s\",\"provision_stage\":\"%s\",\"uptime_s\":%lld,\"reset_reason\":%d,"
        "\"usb_stack_min_bytes\":%u,\"internal_heap_free\":%u,\"internal_heap_largest\":%u,"
        "\"fault_source\":\"%s\",\"failed_allocation_bytes\":%u,\"control_max_gap_ms\":%u,"
        "\"utc_synced\":%s,\"utc_ready\":%s,\"utc_epoch_s\":%lld,"
        "\"sntp_enabled\":%s,\"gateway\":\"" IPSTR "\",\"dns\":\"" IPSTR "\","
        "\"fleet_endpoint\":\"%s\",\"fleet_http_status\":%d,\"fleet_detail\":\"%s\",\"gps_source_value\":%.17g,\"gps_source_text\":\"%s\",\"fleet_txid\":\"%s\",\"fleet_date\":\"%s\",\"fleet_received_utc_s\":%lld,\"report_timestamp_text\":\"%s\",\"api_version\":%lld,"
        "\"wifi_connected\":%s,\"ip\":\"" IPSTR "\",\"station_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
        read_name(raw),valid?"true":"false",read_name(marker),pending?"true":"false",
        token==Error::None?"usable":token==Error::Reauthorize?"missing_or_reauthorize":"error",
        provision_stage,static_cast<long long>(now_ms()/1000),int(esp_reset_reason()),
        unsigned(uxTaskGetStackHighWaterMark(nullptr)),unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),cause?cause:"none",
        unsigned(failed_allocation_bytes.load()),unsigned(control_max_gap_ms.load()),
        utc_synced?"true":"false",state.utc_ok?"true":"false",static_cast<long long>(time(nullptr)),
        esp_sntp_enabled()?"true":"false",IP2STR(&ip.gw),IP2STR(&dns.ip.u_addr.ip4),
        state.fleet.endpoint,state.fleet.http_status,state.fleet.detail,state.fleet.gps_source_value,state.fleet.gps_source_text,
        state.fleet.transaction_id,state.fleet.response_date,static_cast<long long>(state.fleet.received_utc_s),
        state.fleet.vehicle_metadata.report_timestamp_text,static_cast<long long>(state.fleet.vehicle_metadata.api_version),
        wifi_connected?"true":"false",IP2STR(&ip.ip),mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    mbedtls_platform_zeroize(&journal,sizeof journal);
    usb_serial_jtag_write_bytes(b,std::strlen(b),pdMS_TO_TICKS(1000));
    usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(1000));
}
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
    provision_stage="stopping_network";
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
    provision_stage="password_hash";
    ok=ok && password_hash(pw,next.salt,next.password_hash);
    provision_stage="profile_validation";
    ok=ok && valid_profile(next);
    if(ok) {
        uint8_t pending=1;
        provision_stage="pending_commit";
        ok=storage().write("provisioning",&pending,sizeof pending);
        TokenJournal journal(storage());
        if(ok) {provision_stage="token_commit";ok=journal.provision(refresh)==Error::None;}
        if(ok) {provision_stage="profile_commit";ok=save_profile(next);}
        pending=0;
        if(ok) {provision_stage="completion_commit";ok=storage().write("provisioning",&pending,sizeof pending);}
        mbedtls_platform_zeroize(&journal,sizeof journal);
        if(!ok)fail("provision_write");
    }
    mbedtls_platform_zeroize(pw,sizeof pw);mbedtls_platform_zeroize(refresh,sizeof refresh);
    mbedtls_platform_zeroize(&next,sizeof next);
    if(ok)provision_stage="complete";
    return ok;
}
void process(const char* input) {
    // The USB task serializes requests. Keep the 6 KiB parser off its call stack:
    // token/NVS operations need their own stack while this parse remains alive.
    static Json j;
    if(!j.parse(input)) {send("{\"ok\":false,\"error\":\"invalid_json\"}");return;}
    int op=j.get(0,"op");
    if(j.equal(op,"hello")) {
        send("{\"protocol\":1,\"board\":\"ESP32-S3-Relay-1CH\",\"firmware\":\"0.1.0\",\"secrets_echoed\":false}");return;
    }
    if(j.equal(op,"diagnostics")) {diagnostics();return;}
    if(j.equal(op,"status")) {
        auto s=snapshot();char b[256]={};
        std::snprintf(b,sizeof b,"{\"ready\":%s,\"commissioned\":%s,\"disabled\":%s,\"dry_run\":%s,\"commanded_on\":%s,\"fault\":%s}",
            s.ready?"true":"false",s.config.commissioned?"true":"false",s.config.disabled?"true":"false",
            s.config.dry_run?"true":"false",s.decision.commanded?"true":"false",critical_fault?"true":"false");
        send(b);return;
    }
    if(j.equal(op,"wifi_scan")) {
        static char response[3072];
        scan_unprovisioned_wifi(response,sizeof response);send(response);return;
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
    if(usb_serial_jtag_driver_install(&c)!=ESP_OK) {fail("usb_driver");vTaskDelete(nullptr);return;}
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
