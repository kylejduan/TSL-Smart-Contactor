#include "runtime.hpp"
#include "storage.hpp"
#include "board.hpp"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/queue.h"
#include <cstring>
#include <ctime>
namespace app {
std::atomic<bool> critical_fault{false},wifi_connected{false},utc_synced{false},provisioning{false},network_busy{false};
std::atomic<bool> check_requested{false};
std::atomic<const char*> fault_source{nullptr};
std::atomic<uint32_t> failed_allocation_bytes{0},control_max_gap_ms{0};
void fail(const char* reason) {
    const char* empty=nullptr;
    fault_source.compare_exchange_strong(empty,reason);
    critical_fault=true;
}
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static Snapshot state;
static bool forced_off=false,pending_config=false,pending_timed=false;
static Config new_config;
static uint32_t config_epoch=0,timed_epoch=0,timed_seconds=0;
static StaticQueue_t obs_queue_data;
static uint8_t obs_queue_buffer[8*sizeof(Observation)];
static QueueHandle_t obs_queue=nullptr;
Ms now_ms() {return esp_timer_get_time()/1000;}
Snapshot snapshot() {
    portENTER_CRITICAL(&lock);auto copy=state;portEXIT_CRITICAL(&lock);return copy;
}
uint32_t inhibit() {
    portENTER_CRITICAL(&lock);
    state.inhibited=true;forced_off=true;state.config.disabled=true;
    ++state.generation;pending_config=false;pending_timed=false;
    auto epoch=state.generation;
    portEXIT_CRITICAL(&lock);return epoch;
}
bool configure(const Config& c,uint32_t epoch) {
    portENTER_CRITICAL(&lock);
    bool ok=epoch==state.generation;
    if(ok) {new_config=c;config_epoch=epoch;pending_config=true;}
    portEXIT_CRITICAL(&lock);return ok;
}
bool timed(uint32_t seconds,uint32_t epoch) {
    portENTER_CRITICAL(&lock);
    bool ok=state.ready && !state.inhibited && !state.config.disabled && state.config.commissioned &&
        epoch==state.generation && seconds>0 && seconds<=28800;
    if(ok) {timed_seconds=seconds;timed_epoch=epoch;pending_timed=true;}
    portEXIT_CRITICAL(&lock);return ok;
}
bool submit(const Observation& o) {
    if(!obs_queue || xQueueSend(obs_queue,&o,0)!=pdTRUE) {fail("observation_queue");return false;}
    return true;
}
void network_status(Vehicle v,Error e,Ms last,Ms next,const BudgetRecord& b,bool paused) {
    portENTER_CRITICAL(&lock);
    state.vehicle=v;state.error=e;state.last_poll=last;state.next_poll=next;state.budget=b;state.polling_paused=paused;
    portEXIT_CRITICAL(&lock);
}
static void allocation_failed(size_t size,uint32_t,const char*) {
    failed_allocation_bytes=static_cast<uint32_t>(size);fail("allocation_failed");
}
static void setup(void*) {
    static Profile profile;
    if(!storage().initialize())fail("storage_init");
    auto r=load_profile(profile);
    if(r==ReadResult::Failed)fail("profile_load");
    // Provisioning nests certificate validation and NVS writes. Keep measured
    // headroom beyond their buffers; the USB diagnostics expose the watermark.
    if(xTaskCreate(usb_task,"usb_provision",32768,nullptr,2,nullptr)!=pdPASS)fail("setup_or_usb_task");
    if(r==ReadResult::Ok && valid_profile(profile) && !critical_fault) {
        configure(profile.config,snapshot().generation);
        start_wifi(profile);
        if(!start_management(profile))fail("https_start");
        start_tesla(profile);
    } else if(r==ReadResult::Ok)fail("profile_validation");
    vTaskDelete(nullptr);
}
}
extern "C" void app_main() {
    using namespace app;
    // app_main remains the ONE control task and the only owner of this GPIO.
    if(!board::initialize_off())fail("gpio_init");
    if(!esp_psram_is_initialized() || esp_psram_get_size()<8*1024*1024)fail("psram_init");
    vTaskPrioritySet(nullptr,10);
    if(heap_caps_register_failed_alloc_callback(allocation_failed)!=ESP_OK)fail("allocation_monitor");
    if(esp_task_wdt_add(nullptr)!=ESP_OK)fail("watchdog_init");
    obs_queue=xQueueCreateStatic(8,sizeof(Observation),obs_queue_buffer,&obs_queue_data);
    if(!obs_queue || xTaskCreatePinnedToCore(setup,"setup",12288,nullptr,2,nullptr,0)!=pdPASS)fail("setup_or_usb_task");
    Policy policy(now_ms());ClockGuard clock;
    TickType_t wake=xTaskGetTickCount();Ms last_tick=now_ms();
    Reason last_reason=Reason::NoAuthorization;bool last_command=false;
    while(true) {
        Ms now=now_ms();
        auto gap=static_cast<uint32_t>(now-last_tick);
        if(gap>control_max_gap_ms)control_max_gap_ms=gap;
        if(gap>250)fail("control_deadline");
        last_tick=now;
        bool off,apply,override;Config c;uint32_t epoch,seconds,te;
        portENTER_CRITICAL(&lock);
        off=forced_off;forced_off=false;epoch=state.generation;
        apply=pending_config && config_epoch==epoch;c=new_config;pending_config=false;
        override=pending_timed;seconds=timed_seconds;te=timed_epoch;pending_timed=false;
        if(apply) {state.config=c;state.ready=true;state.inhibited=false;}
        portEXIT_CRITICAL(&lock);
        if(off)policy.off(epoch,now);
        if(apply)policy.configure(c,epoch,now);
        if(override && te==epoch)policy.timed_on(seconds,now);
        bool time_ok=clock.update(now,time(nullptr),utc_synced);
        if(clock.jumped())policy.clock_discontinuity(now);
        if(critical_fault)policy.fault(now);
        Observation o;
        for(int i=0;i<8 && xQueueReceive(obs_queue,&o,0)==pdTRUE;++i)
            policy.observe(o,now,time(nullptr),time_ok);
        Decision d=policy.tick(now);
        portENTER_CRITICAL(&lock);
        if(state.inhibited || state.generation!=policy.generation() || critical_fault || provisioning)d.commanded=false;
        if(!board::command(d.commanded)) {fail("gpio_command");board::command(false);d.commanded=false;}
        state.decision=d;state.utc_ok=time_ok;
        if(d.reason!=last_reason || d.commanded!=last_command) {
            state.events[state.event_count%16]={now,d.reason,d.commanded};++state.event_count;
            last_reason=d.reason;last_command=d.commanded;
        }
        portEXIT_CRITICAL(&lock);
        // Feed only after this task checked deadlines, processed OFF, commanded GPIO
        // and published state. A hung network task cannot feed this watchdog.
        if(esp_task_wdt_reset()!=ESP_OK)fail("watchdog_feed");
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(50));
    }
}
