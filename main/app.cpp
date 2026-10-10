#include "runtime.hpp"
#include "storage.hpp"
#include "ota.hpp"
#include "board.hpp"
#include "control_epoch.hpp"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/queue.h"
#include <cstring>
#include <ctime>
namespace app {
std::atomic<bool> critical_fault{false},wifi_connected{false},utc_synced{false},utc_continuity{false},provisioning{false},network_busy{false};
std::atomic<bool> check_requested{false};
std::atomic<const char*> fault_source{nullptr};
std::atomic<uint32_t> failed_allocation_bytes{0},control_max_gap_ms{0};
std::atomic<uint32_t> utc_last_sync_uptime_s{0};
std::atomic<uint32_t> wifi_disconnect_reason{0},wifi_connect_error{0},wifi_connect_attempts{0};
void fail(const char* reason) {
    const char* empty=nullptr;
    fault_source.compare_exchange_strong(empty,reason);
    critical_fault=true;
}
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static Snapshot state;
static ControlEpoch control_epoch;
static bool forced_off=false,pending_config=false,pending_timed=false;
static Config new_config;
static AutoState pending_restored=AutoState::Unknown;
static int64_t pending_source=0;
static uint32_t config_epoch=0,timed_epoch=0,timed_seconds=0;
static StaticQueue_t obs_queue_data;
static uint8_t obs_queue_buffer[8*sizeof(Observation)];
static QueueHandle_t obs_queue=nullptr;
Ms now_ms() {return esp_timer_get_time()/1000;}
Snapshot snapshot() {
    portENTER_CRITICAL(&lock);auto copy=state;copy.generation=control_epoch.current();
    portEXIT_CRITICAL(&lock);return copy;
}
static void inhibit_locked() {
    state.inhibited=true;forced_off=true;state.config.disabled=true;
    pending_config=false;pending_timed=false;
}
uint32_t inhibit() {
    portENTER_CRITICAL(&lock);
    auto epoch=control_epoch.cancel();inhibit_locked();
    portEXIT_CRITICAL(&lock);return epoch;
}
bool inhibit_current(uint32_t expected,uint32_t& acquired) {
    portENTER_CRITICAL(&lock);
    bool ok=!provisioning && !firmware_busy && control_epoch.begin(expected,acquired);
    if(ok)inhibit_locked();
    portEXIT_CRITICAL(&lock);return ok;
}
bool begin_provision(uint32_t expected,uint32_t& acquired) {
    portENTER_CRITICAL(&lock);
    bool ok=!firmware_busy && (expected ? control_epoch.begin(expected,acquired) : true);
    if(ok) {
        if(!expected)acquired=control_epoch.cancel();
        inhibit_locked();provisioning=true;
    }
    portEXIT_CRITICAL(&lock);return ok;
}
bool configure(const Config& c,uint32_t epoch,AutoState restored,int64_t source) {
    portENTER_CRITICAL(&lock);
    bool ok=control_epoch.matches(epoch);
    if(ok) {
        if(!c.disabled || std::strncmp(c.vin,state.config.vin,sizeof c.vin) ||
           c.home_lat!=state.config.home_lat || c.home_lon!=state.config.home_lon) {
            state.vehicle=Vehicle::Unknown;state.error=Error::None;
            state.last_poll=0;state.next_poll=0;state.polling_paused=false;state.fleet={};
        }
        new_config=c;config_epoch=epoch;pending_config=true;
        pending_restored=restored;
        pending_source=source;
    }
    portEXIT_CRITICAL(&lock);return ok;
}
void auto_commit_ack(const Snapshot& committed) {
    portENTER_CRITICAL(&lock);
    if(!critical_fault && !provisioning && !state.inhibited &&
       control_epoch.matches(committed.generation) &&
       state.decision.auto_state==committed.decision.auto_state) {
        state.auto_saved=committed.decision.auto_state;
        state.auto_saved_generation=committed.generation;
    }
    portEXIT_CRITICAL(&lock);
}
bool timed(uint32_t seconds,uint32_t epoch) {
    portENTER_CRITICAL(&lock);
    bool ok=!firmware_busy && state.ready && !state.inhibited && !state.config.disabled && state.config.commissioned &&
        control_epoch.matches(epoch) && seconds>0 && seconds<=28800;
    if(ok) {timed_seconds=seconds;timed_epoch=epoch;pending_timed=true;}
    portEXIT_CRITICAL(&lock);return ok;
}
bool submit(const Observation& o) {
    if(!obs_queue || xQueueSend(obs_queue,&o,0)!=pdTRUE) {fail("observation_queue");return false;}
    return true;
}
void network_status(uint32_t generation,Vehicle v,Error e,Ms last,Ms next,const BudgetRecord& b,bool paused,FleetDiagnostics diagnostic) {
    portENTER_CRITICAL(&lock);
    // Request reservations are global accounting, even while the user has OFF
    // selected. Vehicle evidence and its diagnostics belong to one generation.
    if(control_epoch.matches(generation))state.budget=b;
    // A persisted revoked-token state must remain visible after a DISABLED boot,
    // even though no vehicle request or location diagnostic is published there.
    if(control_epoch.matches(generation) && state.config.disabled && e==Error::Reauthorize)
        state.error=e;
    if(control_epoch.matches(generation) && !state.inhibited && !state.config.disabled) {
        state.vehicle=v;state.error=e;state.last_poll=last;state.next_poll=next;
        state.polling_paused=paused;state.fleet=diagnostic;
    }
    portEXIT_CRITICAL(&lock);
}
static void allocation_failed(size_t size,uint32_t,const char*) {
    // A rejected TLS connection/login allocation is a recoverable request
    // failure, not evidence that the control path is broken. Its caller must
    // check the error; startup, task, storage and GPIO failures still fail OFF.
    failed_allocation_bytes=static_cast<uint32_t>(size);
}
static void setup(void*) {
    static Profile profile;
    const auto startup_generation=snapshot().generation;
    if(!storage().initialize())fail("storage_init");
    auto r=load_profile(profile);
    if(r==ReadResult::Failed)fail("profile_load");
    // Provisioning nests certificate validation and NVS writes. Keep measured
    // headroom beyond their buffers; the USB diagnostics expose the watermark.
    if(xTaskCreate(usb_task,"usb_provision",32768,nullptr,2,nullptr)!=pdPASS)fail("setup_or_usb_task");
    if(!start_ota())fail("ota_task");
    if(r==ReadResult::Ok && valid_profile(profile) && !critical_fault) {
        AutoState saved=AutoState::Unknown;
        int64_t source=0;
        const auto reset=esp_reset_reason();
        // A watchdog/panic indicates a local control failure, not an ordinary
        // outage. Discard its cached permission durably before continuing.
        bool resume=reset==ESP_RST_POWERON || reset==ESP_RST_EXT || reset==ESP_RST_SW ||
            reset==ESP_RST_BROWNOUT || reset==ESP_RST_PWR_GLITCH;
        if(load_auto_state(profile.config,saved,source,resume)!=Error::None)fail("auto_state_load");
        // USB OFF may have superseded this profile while it was being validated.
        if(!critical_fault)configure(profile.config,startup_generation,saved,source);
        if(!start_auto_storage())fail("auto_storage_task");
        if(!provisioning) {
            // Reserve the largest internal-RAM stack before Wi-Fi/HTTPS split
            // the remaining heap into blocks too small for its 64 KiB allocation.
            // The worker cannot contact Fleet before Wi-Fi/time/config are ready.
            start_tesla(profile);
            start_wifi(profile);
            if(!start_management(profile))fail("https_start");
        }
    } else if(r==ReadResult::Ok)fail("profile_validation");
    setup_complete=true;
    vTaskDelete(nullptr);
}
}
extern "C" void app_main() {
    using namespace app;
    // app_main remains the ONE control task and the only owner of this GPIO.
    if(!board::initialize_off())fail("gpio_init");
    ota_boot_initialize();
    // Do not reuse a predictable generation from a previous boot's client page.
    // This is a command-order marker, separate from authentication/session keys.
    control_epoch=ControlEpoch(esp_random());
    if(!esp_psram_is_initialized() || esp_psram_get_size()<8*1024*1024)fail("psram_init");
    vTaskPrioritySet(nullptr,10);
    if(heap_caps_register_failed_alloc_callback(allocation_failed)!=ESP_OK)fail("allocation_monitor");
    if(esp_task_wdt_add(nullptr)!=ESP_OK)fail("watchdog_init");
    obs_queue=xQueueCreateStatic(8,sizeof(Observation),obs_queue_buffer,&obs_queue_data);
    if(!obs_queue || xTaskCreatePinnedToCore(setup,"setup",20480,nullptr,2,nullptr,0)!=pdPASS)fail("setup_or_usb_task");
    Policy policy(now_ms());ClockGuard clock;
    TickType_t wake=xTaskGetTickCount();Ms last_tick=now_ms();
    Reason last_reason=Reason::NoAuthorization;bool last_command=false;
    while(true) {
        Ms now=now_ms();
        auto gap=static_cast<uint32_t>(now-last_tick);
        if(gap>control_max_gap_ms)control_max_gap_ms=gap;
        if(gap>250)fail("control_deadline");
        last_tick=now;
        bool off,apply,override;Config c;uint32_t epoch,seconds,te;AutoState restored;int64_t source;
        portENTER_CRITICAL(&lock);
        off=forced_off;forced_off=false;epoch=control_epoch.current();
        apply=pending_config && config_epoch==epoch;c=new_config;pending_config=false;
        restored=pending_restored;
        source=pending_source;
        override=pending_timed;seconds=timed_seconds;te=timed_epoch;pending_timed=false;
        if(apply) {
            state.config=c;state.ready=true;state.inhibited=false;
            // Publish the new generation's empty/restored decision atomically
            // with its configuration. A storage worker must never bind the
            // previous HOME decision to newly configured VIN/home settings.
            state.decision={};state.decision.auto_state=restored;
            state.auto_saved=restored;state.auto_saved_generation=epoch;
        }
        portEXIT_CRITICAL(&lock);
        if(off)policy.off(epoch,now);
        if(apply) {policy.configure(c,epoch,now);policy.restore(restored,source);}
        if(override && te==epoch)policy.timed_on(seconds,now);
        // Keep an existing monotonic lease through a brief Wi-Fi outage, but
        // require a new SNTP sync before admitting another location fix.
        bool time_ok=clock.update(now,time(nullptr),utc_continuity);
        bool evidence_time_ok=time_ok && wifi_connected &&
            recent_utc_sync(static_cast<uint32_t>(now/1000),utc_last_sync_uptime_s,utc_synced);
        if(clock.jumped())policy.clock_discontinuity(now);
        if(critical_fault)policy.fault(now);
        Observation o;
        for(int i=0;obs_queue && i<8 && xQueueReceive(obs_queue,&o,0)==pdTRUE;++i)
            policy.observe(o,now,time(nullptr),evidence_time_ok);
        const auto committed=snapshot();
        bool commit_ready=effective_outage_policy(committed.config)!=OutagePolicy::HoldLast ||
            (committed.auto_saved_generation==policy.generation() && committed.auto_saved==AutoState::Home);
        Decision d=policy.tick(now,commit_ready,firmware_busy);
        portENTER_CRITICAL(&lock);
        if(state.inhibited || !control_epoch.matches(policy.generation()) || critical_fault || provisioning || firmware_busy)d.commanded=false;
        if(!board::command(d.commanded)) {fail("gpio_command");board::command(false);d.commanded=false;}
        state.decision=d;state.utc_ok=evidence_time_ok;
        if(d.reason!=last_reason || d.commanded!=last_command) {
            state.events.record(now,d.reason,d.commanded);
            last_reason=d.reason;last_command=d.commanded;
        }
        portEXIT_CRITICAL(&lock);
        // Feed only after this task checked deadlines, processed OFF, commanded GPIO
        // and published state. A hung network task cannot feed this watchdog.
        if(esp_task_wdt_reset()!=ESP_OK)fail("watchdog_feed");
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(50));
    }
}
