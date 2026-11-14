// Actual Fleet worker sequencing with synthetic storage, time and transport.
#include "runtime_sdk_stubs.hpp"
#include "storage.hpp"
#include "network.hpp"
#include "ota.hpp"
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#define CHECK(v) do {if(!(v))throw std::runtime_error(#v);}while(false)
namespace {
struct Stop {};
std::string scenario;
tsl::Ms now=0,reconnect_at=0;
app::Snapshot state;
app::NvsStore store;
std::map<std::string,std::vector<char>> records;
TaskFunction_t worker=nullptr;
unsigned requests=0,recoveries=0,submissions=0;
}
namespace app {
std::atomic<bool> critical_fault{false},wifi_connected{true},utc_synced{true},utc_continuity{true},provisioning{false},network_busy{false},firmware_busy{false},check_requested{false};
Snapshot snapshot() {return state;}
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
bool submit(const Observation&) {++submissions;return true;}
void request(Endpoint endpoint,const Config&,const char*,const char*,HttpResult& result) {
    CHECK(endpoint==Endpoint::Refresh && network_busy);++requests;now+=10;
    result.error=scenario=="server" ? Error::Server : Error::Timeout;
    result.status=scenario=="server" ? 503 : 0;
    result.request_may_have_been_sent=scenario=="ambiguous" || scenario=="server";
    result.transport.phase=result.request_may_have_been_sent ? "read_response" : "connect";
    result.transport.request_may_have_been_sent=result.request_may_have_been_sent;
    if(scenario=="certificate") {result.error=Error::Transport;result.transport.mbedtls_error=-0x2700;result.transport.verify_flags=8;}
    if(requests==3 && scenario=="off") {++state.generation;state.config.disabled=true;}
    if(requests==3 && scenario=="ota")firmware_busy=true;
}
esp_err_t recover_station_network() {
    CHECK(network_busy && !firmware_busy && !provisioning && !critical_fault && !state.config.disabled);
    CHECK(requests==3 && now>=180000);++recoveries;
    wifi_connected=false;utc_synced=false;state.utc_ok=false;reconnect_at=now+500;
    return ESP_OK;
}
void network_status(uint32_t,Vehicle vehicle,Error error,Ms last,Ms next,const BudgetRecord& b,bool paused,FleetDiagnostics diagnostic) {
    state.vehicle=vehicle;state.error=error;state.last_poll=last;state.next_poll=next;
    state.budget=b;state.polling_paused=paused;state.fleet=diagnostic;
}
}
extern "C" time_t __wrap_time(time_t* out) {
    const auto value=time_t(1800000000+now/1000);if(out)*out=value;return value;
}
uint32_t esp_random() {return 0;}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {return 16384;}
BaseType_t xTaskCreate(TaskFunction_t task,const char* name,uint32_t,void*,UBaseType_t,TaskHandle_t* handle) {
    CHECK(std::string(name)=="tesla_worker");worker=task;if(handle)*handle=&store;return pdPASS;
}
void vTaskDelay(TickType_t ticks) {
    now+=ticks;
    if(reconnect_at && now>=reconnect_at) {app::wifi_connected=true;app::utc_synced=true;state.utc_ok=true;reconnect_at=0;}
    if(now>=250000)throw Stop{};
}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);scenario=argv[1];state.ready=true;state.inhibited=false;state.utc_ok=true;
        state.config.commissioned=true;state.config.disabled=false;state.config.dry_run=false;
        state.decision.commanded=true;state.decision.auto_home=true;
        std::strcpy(state.config.vin,"5YJ3E1EA7KF000001");
        {tsl::TokenJournal journal(store);CHECK(journal.provision("synthetic-original")==tsl::Error::None);}
        app::Profile profile;std::strcpy(profile.client_id,"synthetic-client");
        app::start_tesla(profile);CHECK(worker);
        try {worker(&profile);}catch(const Stop&) {}
        CHECK(requests==3 && !app::network_busy && !app::critical_fault && !submissions);
        CHECK(recoveries==(scenario=="unsent" ? 1u : 0u));
        CHECK(state.fleet.network_recoveries==recoveries);
        CHECK(state.decision.commanded && state.decision.auto_home); // Recovery has no GPIO/policy action.
        CHECK(state.budget.monthly[1]==0 && state.budget.monthly[2]==4);
        tsl::TokenJournal journal(store);CHECK(journal.load()==tsl::Error::None);
        CHECK(std::string(journal.current())=="synthetic-original");
        std::cout<<"PASS "<<scenario<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
