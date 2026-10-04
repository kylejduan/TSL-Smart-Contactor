#pragma once
#include "policy.hpp"
#include "reliability.hpp"
#include "client.hpp"
#include "events.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
namespace app {
using namespace tsl;
struct Profile {
    uint32_t version=2;
    Config config{};
    char ssid[33]={}, wifi_password[65]={}, client_id[129]={};
    uint8_t salt[16]={}, password_hash[32]={};
    char certificate[2049]={}, private_key[4097]={}, origin[193]={};
    uint32_t crc=0;
};
// Config schema 2 uses one former padding byte. Existing NVS profiles retain
// their exact layout/CRC coverage; schema 1 always selects strict GPS time.
static_assert(sizeof(Profile)==6720 && offsetof(Profile,config)==8 &&
              offsetof(Profile,ssid)==96 && offsetof(Profile,crc)==6712,
              "Stored profile layout changed; an explicit migration is required");
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
    EventLog events{};
};
extern std::atomic<bool> critical_fault, wifi_connected, utc_synced, utc_continuity, provisioning, network_busy;
extern std::atomic<bool> check_requested;
extern std::atomic<const char*> fault_source;
extern std::atomic<uint32_t> failed_allocation_bytes, control_max_gap_ms;
extern std::atomic<uint32_t> wifi_disconnect_reason, wifi_connect_error, wifi_connect_attempts;
void fail(const char* static_reason);
Ms now_ms();
Snapshot snapshot();
uint32_t inhibit(); // Immediate OFF + invalidates in-flight and queued requests
bool inhibit_current(uint32_t expected,uint32_t& acquired); // Reject stale non-OFF commands
bool begin_provision(uint32_t expected,uint32_t& acquired); // 0 only for full USB replacement
bool configure(const Config&,uint32_t epoch);
bool timed(uint32_t seconds,uint32_t epoch);
bool submit(const Observation&);
void network_status(uint32_t generation,Vehicle,Error,Ms last,Ms next,const BudgetRecord&,bool paused,FleetDiagnostics);
void start_wifi(const Profile&);
void start_tesla(const Profile&);
bool start_management(Profile&);
void usb_task(void*);
size_t status_json(char* out,size_t capacity);
}
