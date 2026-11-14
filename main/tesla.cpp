#include "network.hpp"
#include "ota.hpp"
#include "storage.hpp"
#include "client.hpp"
#include "esp_random.h"
#include <cstring>
#include <ctime>
namespace app {
namespace {
struct RuntimeIO : FleetIO {
    Ms now() const override {return now_ms();}
    int64_t utc() const override {return time(nullptr);}
    bool ready() const override {return snapshot().utc_ok && wifi_connected;}
    bool current(uint32_t generation) const override {
        auto s=snapshot();return s.generation==generation && !s.inhibited && !s.config.disabled && !provisioning && !firmware_busy;
    }
    void request(Endpoint e,const Config& c,const char* access,const char* form,HttpResult& result) override {
        app::request(e,c,access,form,result);
    }
};
void worker(void* context) {
    auto& profile=*static_cast<const Profile*>(context);
    RuntimeIO io;FleetClient client(storage(),io,profile.client_id);
    Scheduler scheduler;TransportRecovery recovery;uint64_t sequence=0;
    uint32_t recovery_uptime_s=0;int recovery_error=0;
    auto initial=client.initialize();
    if(initial==Error::Storage)fail("tesla_journal_load");
    Ms last_success=0,auth_retry_after=0;Vehicle vehicle=Vehicle::Unknown;Error error=initial;
    uint32_t view_generation=snapshot().generation;
    while(true) {
        auto s=snapshot();
        // Tesla restores billing-limit access at a new UTC billing month.
        // Only synchronized time can release that specific scheduler pause;
        // permission/revocation/storage pauses remain latched.
        if(io.ready()) {
            time_t utc=io.utc();tm calendar{};
            if(gmtime_r(&utc,&calendar))
                scheduler.calendar_month((calendar.tm_year+1900)*12+calendar.tm_mon+1,now_ms());
        }
        if(s.generation!=view_generation) {
            view_generation=s.generation;
            vehicle=Vehicle::Unknown;
            error=client.reauthorization_needed() ? Error::Reauthorize : Error::None;
            last_success=0;
            client.clear_diagnostics();
            recovery.clear_streak();
        }
        if(check_requested.exchange(false))scheduler.check_now(now_ms());
        if(!firmware_busy && !provisioning && !critical_fault && s.ready && !s.config.disabled) {
            bool poll=scheduler.due(now_ms());
            bool refresh=client.refresh_due() && !scheduler.paused() && now_ms()>=auth_retry_after && io.ready();
            if(poll || refresh) {
                network_busy=true;
                if(provisioning || firmware_busy) {network_busy=false;continue;}
                Observation o;o.generation=s.generation;o.request=++sequence;std::strcpy(o.vin,s.config.vin);
                if(poll) {
                    scheduler.begin(now_ms());error=client.poll(s.config,s.generation,o);
                    if(o.vehicle!=Vehicle::Unknown && io.current(s.generation))vehicle=o.vehicle;
                } else error=client.refresh(s.config);
                if(error==Error::Storage)fail("tesla_storage");
                if(error==Error::Reauthorize || error==Error::Authentication || error==Error::Permission) {
                    o.kind=Evidence::Revoked;submit(o);
                } else if(error==Error::None && poll) {
                    last_success=now_ms();submit(o);
                }
                if(poll || error!=Error::None) {
                    scheduler.finish(now_ms(),error,s.config.poll_s,client.retry_s(),esp_random());
                    auth_retry_after=error==Error::None ? 0 : scheduler.next();
                }
                if(io.current(s.generation) &&
                   recovery.observe(error,client.diagnostics().transport,now_ms())) {
                    recovery_uptime_s=uint32_t(now_ms()/1000);
                    recovery_error=recover_station_network();
                }
                network_busy=false;
            }
        }
        auto diagnostic=client.diagnostics();
        diagnostic.network_recoveries=recovery.attempts();
        diagnostic.last_network_recovery_uptime_s=recovery_uptime_s;
        diagnostic.network_recovery_error=recovery_error;
        network_status(s.generation,vehicle,error,last_success,scheduler.next(),client.counts(),scheduler.paused(),diagnostic);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
std::atomic<TaskHandle_t> tesla_task{nullptr};
}
void start_tesla(const Profile& p) {
    TaskHandle_t task=nullptr;
    if(xTaskCreate(worker,"tesla_worker",65536,const_cast<Profile*>(&p),3,&task)!=pdPASS)fail("tesla_task");
    tesla_task.store(task);
}
uint32_t tesla_stack_min_bytes() {
    auto task=tesla_task.load();return task ? uxTaskGetStackHighWaterMark(task) : 0;
}
}
