#pragma once
#include "policy.hpp"
#include "reliability.hpp"
#include "client.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
namespace app {
using namespace tsl;
struct Profile {
    uint32_t version=1;
    Config config{};
    char ssid[33]={}, wifi_password[65]={}, client_id[129]={};
    uint8_t salt[16]={}, password_hash[32]={};
    char certificate[2049]={}, private_key[4097]={}, origin[193]={};
    uint32_t crc=0;
};
struct Event { Ms at=0; Reason reason=Reason::NoAuthorization; bool commanded=false; };
struct Snapshot {
    Config config{};
    Decision decision{};
    uint32_t generation=1;
    bool ready=false, utc_ok=false, inhibited=true, polling_paused=false;
    Vehicle vehicle=Vehicle::Unknown;
    Error error=Error::None;
    Ms last_poll=0,next_poll=0;
    BudgetRecord budget{};
    FleetDiagnostics fleet{};
    Event events[16]{};
    unsigned event_count=0;
};
extern std::atomic<bool> critical_fault, wifi_connected, utc_synced, provisioning, network_busy;
extern std::atomic<bool> check_requested;
extern std::atomic<const char*> fault_source;
extern std::atomic<uint32_t> failed_allocation_bytes, control_max_gap_ms;
void fail(const char* static_reason);
Ms now_ms();
Snapshot snapshot();
uint32_t inhibit(); // Immediate OFF + invalidates in-flight and queued requests
bool configure(const Config&,uint32_t epoch);
bool timed(uint32_t seconds,uint32_t epoch);
bool submit(const Observation&);
void network_status(Vehicle,Error,Ms last,Ms next,const BudgetRecord&,bool paused,FleetDiagnostics);
void start_wifi(const Profile&);
void start_tesla(const Profile&);
bool start_management(const Profile&);
void usb_task(void*);
size_t status_json(char* out,size_t capacity);
}
