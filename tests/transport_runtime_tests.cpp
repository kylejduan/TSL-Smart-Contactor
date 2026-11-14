// Run the production ESP-TLS adapter unchanged against bounded synthetic I/O.
#include "transport_sdk_stubs.hpp"
#include "network.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#define CHECK(v) do {if(!(v))throw std::runtime_error(#v);}while(false)
namespace {
std::string scenario,wire;
tsl::Ms now=tsl::Ms(30)*86400000; // Exercise uptime beyond signed 32-bit milliseconds.
app::Snapshot state;
esp_tls_t connection;
esp_tls_conn_state_t connect_state=ESP_TLS_INIT;
unsigned allocations=0,destroys=0,writes=0;
size_t received=0,written=0;
int tls_error=0,mbedtls_error=0,verify_flags=0,system_error=0;
bool header_done=false;
unsigned callbacks=0,disconnects=0,dns_clears=0;
bool tcpip_thread=false;
}
namespace app {
std::atomic<bool> wifi_connected{true},utc_synced{true},provisioning{false},critical_fault{false},firmware_busy{false};
Snapshot snapshot() {return state;}
Ms now_ms() {return now;}
}
void vTaskDelay(TickType_t ticks) {now+=ticks;}
esp_err_t esp_crt_bundle_attach(void*) {return ESP_OK;}
esp_tls_t* esp_tls_init() {++allocations;return scenario=="allocation_failure" ? nullptr : &connection;}
int esp_tls_conn_new_async(const char* host,int hostlen,int port,const esp_tls_cfg_t* cfg,esp_tls_t*) {
    CHECK(std::string(host,size_t(hostlen))=="fleet-auth.prd.vn.cloud.tesla.com");
    CHECK(port==443 && cfg->non_block && cfg->timeout_ms==10000 && cfg->crt_bundle_attach);
    if(scenario=="dns_timeout") {now+=10001;tls_error=0x8001;return -1;}
    if(scenario=="connect_error") {connect_state=ESP_TLS_CONNECTING;system_error=EHOSTUNREACH;tls_error=0x8002;return -1;}
    if(scenario=="tls_timeout") {connect_state=ESP_TLS_HANDSHAKE;return 0;}
    if(scenario=="certificate_failure") {
        connect_state=ESP_TLS_FAIL;tls_error=0x8003;mbedtls_error=-0x2700;verify_flags=8;return -1;
    }
    now+=40;connect_state=ESP_TLS_DONE;return 1;
}
ssize_t esp_tls_conn_write(esp_tls_t*,const void* data,size_t size) {
    ++writes;
    const auto view=std::string(static_cast<const char*>(data),size);
    if(scenario=="headers_timeout")return ESP_TLS_ERR_SSL_WANT_WRITE;
    if(scenario=="body_timeout" && header_done)return ESP_TLS_ERR_SSL_WANT_WRITE;
    CHECK(!size || (header_done ? view.find("synthetic")!=std::string::npos : view.find("POST /oauth2/v3/token")!=std::string::npos));
    header_done=true;written+=size;return ssize_t(size);
}
ssize_t esp_tls_conn_read(esp_tls_t*,void* data,size_t size) {
    if(scenario=="read_timeout")return ESP_TLS_ERR_SSL_WANT_READ;
    if(scenario=="read_error") {errno=ECONNRESET;return -0x7200;}
    if(received==wire.size())return ESP_TLS_ERR_SSL_WANT_READ;
    const auto n=std::min({size,wire.size()-received,size_t(7)});
    std::memcpy(data,wire.data()+received,n);received+=n;return ssize_t(n);
}
esp_err_t esp_tls_get_conn_state(esp_tls_t*,esp_tls_conn_state_t* out) {*out=connect_state;return ESP_OK;}
esp_err_t esp_tls_get_error_handle(esp_tls_t*,esp_tls_error_handle_t* out) {*out=&connection;return ESP_OK;}
esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t,esp_tls_error_type_t,int* out) {*out=system_error;system_error=0;return ESP_OK;}
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t,int* code,int* flags) {*code=mbedtls_error;*flags=verify_flags;return tls_error;}
int esp_tls_conn_destroy(esp_tls_t*) {++destroys;return 0;}
void mbedtls_platform_zeroize(void* p,size_t size) {std::memset(p,0,size);}
int tcpip_callback(tcpip_callback_fn fn,void* argument) {
    ++callbacks;if(scenario=="recovery_queue_failure")return ERR_MEM;
    tcpip_thread=true;fn(argument);tcpip_thread=false;return ERR_OK;
}
void dns_clear_cache() {CHECK(tcpip_thread);++dns_clears;}
esp_err_t esp_wifi_disconnect() {++disconnects;return scenario=="recovery_disconnect_failure" ? ESP_FAIL : ESP_OK;}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);scenario=argv[1];state.utc_ok=scenario!="clock";app::wifi_connected=scenario!="wifi";
        if(scenario.rfind("recovery_",0)==0) {
            app::provisioning=scenario=="recovery_provisioning";
            app::firmware_busy=scenario=="recovery_ota";
            app::critical_fault=scenario=="recovery_fault";
            app::wifi_connected=scenario!="recovery_disconnected";
            const auto result=app::recover_station_network();
            if(scenario=="recovery_success")CHECK(result==ESP_OK && callbacks==1 && dns_clears==1 && disconnects==1 && !app::wifi_connected && !app::utc_synced);
            else if(scenario=="recovery_queue_failure")CHECK(result==ESP_ERR_NO_MEM && callbacks==1 && !dns_clears && !disconnects && app::wifi_connected && app::utc_synced);
            else if(scenario=="recovery_disconnect_failure")CHECK(result==ESP_FAIL && callbacks==1 && dns_clears==1 && disconnects==1 && app::wifi_connected && app::utc_synced);
            else CHECK(result==ESP_ERR_INVALID_STATE && !callbacks && !dns_clears && !disconnects);
            std::cout<<"PASS "<<scenario<<'\n';return 0;
        }
        wire=scenario=="partial_response" ? "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{" : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
        tsl::HttpResult result;auto config=tsl::Config{};
        const auto started=now;
        app::request(tsl::Endpoint::Refresh,config,nullptr,"synthetic-form",result);
        const auto& d=result.transport;
        if(scenario=="clock" || scenario=="wifi") {
            CHECK(result.error==(scenario=="clock" ? tsl::Error::Clock : tsl::Error::Wifi));
            CHECK(std::string(d.phase)==scenario && !allocations && !destroys && !result.request_may_have_been_sent);
        } else if(scenario=="allocation_failure") {
            CHECK(result.error==tsl::Error::Transport && std::string(d.phase)=="allocate" && !destroys);
        } else {
            CHECK(allocations==1 && destroys==1 && d.elapsed_ms==now-started);
            CHECK(d.connect_state==connect_state);
            if(scenario=="success") {
                CHECK(result.error==tsl::Error::None && result.status==200 && result.body.view()=="{}");
                CHECK(std::string(d.phase)=="complete" && d.connect_ms==40 && d.bytes_read==wire.size());
            } else if(scenario=="dns_timeout" || scenario=="tls_timeout") {
                CHECK(result.error==tsl::Error::Timeout && std::string(d.phase)=="connect");
                CHECK(d.elapsed_ms>=10000 && !result.request_may_have_been_sent && !writes);
            } else if(scenario=="connect_error" || scenario=="certificate_failure") {
                CHECK(result.error==tsl::Error::Transport && std::string(d.phase)=="connect");
                CHECK(d.tls_error && !result.request_may_have_been_sent && !writes);
                if(scenario=="connect_error")CHECK(d.system_error==EHOSTUNREACH);
                else CHECK(d.mbedtls_error==-0x2700 && d.verify_flags==8);
            } else {
                CHECK(result.request_may_have_been_sent && d.request_may_have_been_sent);
                if(scenario=="read_error")CHECK(result.error==tsl::Error::Transport && d.system_error==ECONNRESET && d.last_io==-0x7200);
                else CHECK(result.error==tsl::Error::Timeout && d.elapsed_ms>=5000 && d.elapsed_ms<10000);
                CHECK(std::string(d.phase)==(scenario=="headers_timeout" ? "write_headers" : scenario=="body_timeout" ? "write_body" : "read_response"));
                if(scenario=="partial_response")CHECK(result.status==200 && d.bytes_read==wire.size());
            }
            CHECK(d.bytes_written==written);
        }
        std::cout<<"PASS "<<scenario<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
