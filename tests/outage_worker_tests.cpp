// Prolonged synthetic outages through the unchanged production Fleet worker.
#include "runtime_sdk_stubs.hpp"
#include "storage.hpp"
#include "network.hpp"
#include "ota.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#define CHECK(v) do {if(!(v))throw std::runtime_error(#v);}while(false)
namespace {
struct Stop {};
constexpr tsl::Ms hour=3600000,start=tsl::Ms(30)*24*hour;
std::string scenario;
tsl::Ms now=start,service_returns=0,reconnect_at=0,first_success=0;
app::Snapshot state;
app::NvsStore store;
tsl::Policy policy;
std::map<std::string,std::vector<char>> records;
TaskFunction_t worker=nullptr;
unsigned requests[3]={},recoveries=0,successful_polls=0;
bool stale_resolver=true;
std::vector<tsl::Ms> recovery_times,failed_refresh_times;
int64_t utc() {return 1800000000+(now-start)/1000;}
bool clock_ready() {return scenario!="clock_48h" || now>=service_returns;}
}
namespace app {
std::atomic<bool> critical_fault{false},wifi_connected{true},utc_synced{true},utc_continuity{true},provisioning{false},network_busy{false},firmware_busy{false},check_requested{false};
Snapshot snapshot() {state.utc_ok=wifi_connected && clock_ready();return state;}
Ms now_ms() {return now;}
NvsStore& storage() {return store;}
ReadResult NvsStore::read(const char* key,void* out,size_t n) {
    auto it=records.find(key);if(it==records.end())return ReadResult::Missing;
    if(it->second.size()!=n)return ReadResult::Failed;
    std::memcpy(out,it->second.data(),n);return ReadResult::Ok;
}
bool NvsStore::write(const char* key,const void* value,size_t n) {
    const auto* bytes=static_cast<const char*>(value);records[key]={bytes,bytes+n};return true;
}
void fail(const char*) {critical_fault=true;}
bool submit(const Observation& o) {
    CHECK(o.generation==state.generation);
    if(o.kind==Evidence::Location) {
        CHECK(now>=service_returns && clock_ready());
        if(!first_success)first_success=now;
        ++successful_polls;
    }
    policy.observe(o,now,utc(),clock_ready());return true;
}
void request(Endpoint endpoint,const Config&,const char* access,const char* form,HttpResult& result) {
    CHECK(network_busy && wifi_connected && clock_ready());
    ++requests[static_cast<size_t>(endpoint)];now+=20;
    const bool down=now<service_returns || (scenario=="connect_48h" && stale_resolver);
    if(down) {
        CHECK(endpoint==Endpoint::Refresh);failed_refresh_times.push_back(now);
        result.error=scenario=="server_8h" || scenario=="retry_after" ? Error::Server : Error::Timeout;
        result.status=result.error==Error::Server ? 503 : 0;
        result.request_may_have_been_sent=scenario!="connect_48h" && scenario!="certificate_8h";
        result.transport.phase=result.request_may_have_been_sent ? "read_response" : "connect";
        result.transport.request_may_have_been_sent=result.request_may_have_been_sent;
        if(scenario=="certificate_8h") {result.error=Error::Transport;result.transport.mbedtls_error=-0x2700;result.transport.verify_flags=8;}
        if(scenario=="retry_after")result.retry_s=7200;
        return;
    }
    char body[512]={};result.status=200;result.request_may_have_been_sent=true;result.transport.phase="complete";
    if(endpoint==Endpoint::Refresh) {
        CHECK(form && !access);
        std::snprintf(body,sizeof body,"{\"access_token\":\"synthetic-access\",\"refresh_token\":\"synthetic-next-%u\",\"token_type\":\"Bearer\",\"expires_in\":3600}",requests[2]);
    } else if(endpoint==Endpoint::Status) {
        CHECK(access && !form);
        std::strcpy(body,"{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\"online\"}}");
    } else {
        std::snprintf(body,sizeof body,"{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":0,\"timestamp\":%lld}}}",static_cast<long long>(utc())*1000);
    }
    CHECK(result.body.append(body,std::strlen(body)));
}
esp_err_t recover_station_network() {
    CHECK(scenario=="connect_48h" && network_busy && wifi_connected && !firmware_busy && !critical_fault);
    CHECK(!policy.tick(now).commanded && !successful_polls);
    if(!recovery_times.empty())CHECK(now-recovery_times.back()>=hour);
    recovery_times.push_back(now);++recoveries;
    if(now>=service_returns)stale_resolver=false;
    wifi_connected=false;utc_synced=false;reconnect_at=now+2000;return ESP_OK;
}
void network_status(uint32_t,Vehicle vehicle,Error error,Ms last,Ms next,const BudgetRecord& b,bool paused,FleetDiagnostics diagnostic) {
    state.vehicle=vehicle;state.error=error;state.last_poll=last;state.next_poll=next;
    state.budget=b;state.polling_paused=paused;state.fleet=diagnostic;
}
}
extern "C" time_t __wrap_time(time_t* out) {const auto value=time_t(utc());if(out)*out=value;return value;}
uint32_t esp_random() {return 30;}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {return 16384;}
BaseType_t xTaskCreate(TaskFunction_t task,const char* name,uint32_t,void*,UBaseType_t,TaskHandle_t* handle) {
    CHECK(std::string(name)=="tesla_worker");worker=task;if(handle)*handle=&store;return pdPASS;
}
void vTaskDelay(TickType_t ticks) {
    now+=ticks;
    if(reconnect_at && now>=reconnect_at) {app::wifi_connected=true;app::utc_synced=true;reconnect_at=0;}
    if(!successful_polls)CHECK(!policy.tick(now).commanded);
    if(successful_polls>=3 || now>=service_returns+5*hour)throw Stop{};
}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);scenario=argv[1];
        CHECK(scenario=="connect_48h" || scenario=="clock_48h" || scenario=="server_8h" || scenario=="certificate_8h" || scenario=="ambiguous_36h" || scenario=="retry_after");
        service_returns=start+(scenario=="connect_48h" || scenario=="clock_48h" ? 48 : scenario=="ambiguous_36h" ? 36 : scenario=="retry_after" ? 2 : 8)*hour;
        state.ready=true;state.inhibited=false;state.generation=1;
        state.config.commissioned=true;state.config.disabled=false;state.config.dry_run=false;
        state.config.outage_policy=uint8_t(tsl::OutagePolicy::HoldLast);
        state.config.position_basis=uint8_t(tsl::PositionBasis::VehicleReport);
        state.config.max_age_s=600;state.config.lease_s=600;state.config.poll_s=540;
        std::strcpy(state.config.vin,"5YJ3E1EA7KF000001");
        policy.configure(state.config,1,now);policy.restore(tsl::AutoState::Away);
        {tsl::TokenJournal journal(store);CHECK(journal.provision("synthetic-original")==tsl::Error::None);}
        app::Profile profile;std::strcpy(profile.client_id,"synthetic-client");
        app::start_tesla(profile);CHECK(worker);
        try {worker(&profile);}catch(const Stop&) {}
        CHECK(!app::network_busy && !app::critical_fault);
        CHECK(state.fleet.network_recoveries==recoveries);
        tsl::TokenJournal journal(store);const auto loaded=journal.load();
        if(scenario=="ambiguous_36h") {
            CHECK(loaded==tsl::Error::Reauthorize && state.error==tsl::Error::Reauthorize);
            CHECK(!successful_polls && !recoveries && !policy.tick(now).commanded);
            CHECK(requests[2]<=tsl::kRefreshRecoveryAttempts && !requests[0] && !requests[1]);
        } else {
            CHECK(loaded==tsl::Error::None && !journal.needs_reauth());
            CHECK(successful_polls==3 && state.error==tsl::Error::None && !state.polling_paused);
            CHECK(policy.tick(now).commanded && policy.tick(now).auto_home);
            CHECK(first_success>=service_returns);
            CHECK(first_success-service_returns<=(scenario=="connect_48h" ? 4*hour+180000 : hour+60000));
            if(scenario=="connect_48h")CHECK(recoveries>1 && !stale_resolver);
            else CHECK(!recoveries);
            if(scenario=="clock_48h")CHECK(failed_refresh_times.empty());
            if(scenario=="retry_after") {
                CHECK(failed_refresh_times.size()==1);
                CHECK(first_success-failed_refresh_times.front()>=2*hour);
            }
        }
        uint64_t daily=0,monthly=0;
        for(int i=0;i<3;++i) {daily+=state.budget.daily[i];monthly+=state.budget.monthly[i];}
        CHECK(daily<=state.config.daily_cap && monthly<=state.config.monthly_cap);
        CHECK(state.budget.monthly[1]<=tsl::kMonthlyDataRequestCap);
        std::cout<<"PASS "<<scenario<<" recoveries="<<recoveries<<" refresh_attempts="<<requests[2]<<" recovery_delay_s="<<(first_success ? (first_success-service_returns)/1000 : -1)<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
