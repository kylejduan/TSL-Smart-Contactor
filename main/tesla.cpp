#include "network.hpp"
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
        auto s=snapshot();return s.generation==generation && !s.inhibited && !s.config.disabled && !provisioning;
    }
    void request(Endpoint e,const Config& c,const char* access,const char* form,HttpResult& result) override {
        app::request(e,c,access,form,result);
    }
};
void worker(void* context) {
    auto& profile=*static_cast<const Profile*>(context);
    RuntimeIO io;FleetClient client(storage(),io,profile.client_id);
    Scheduler scheduler;uint64_t sequence=0;
    auto initial=client.initialize();
    if(initial==Error::Storage)fail("tesla_journal_load");
    Ms last_success=0,auth_retry_after=0;Vehicle vehicle=Vehicle::Unknown;Error error=initial;
    while(true) {
        auto s=snapshot();
        if(check_requested.exchange(false))scheduler.check_now(now_ms());
        if(!provisioning && !critical_fault && s.ready && !s.config.disabled) {
            bool poll=scheduler.due(now_ms());
            bool refresh=client.refresh_due() && !scheduler.paused() && now_ms()>=auth_retry_after && io.ready();
            if(poll || refresh) {
                network_busy=true;
                if(provisioning) {network_busy=false;continue;}
                Observation o;o.generation=s.generation;o.request=++sequence;std::strcpy(o.vin,s.config.vin);
                if(poll) {
                    scheduler.begin(now_ms());error=client.poll(s.config,s.generation,o);
                } else error=client.refresh(s.config);
                if(error==Error::Storage)fail("tesla_storage");
                if(error==Error::Reauthorize || error==Error::Authentication || error==Error::Permission) {
                    o.kind=Evidence::Revoked;submit(o);
                } else if(error==Error::None && poll) {
                    last_success=now_ms();vehicle=o.vehicle;submit(o);
                }
                if(poll || error!=Error::None) {
                    scheduler.finish(now_ms(),error,s.config.poll_s,client.retry_s(),esp_random());
                    auth_retry_after=error==Error::None ? 0 : scheduler.next();
                }
                network_busy=false;
            }
        }
        network_status(vehicle,error,last_success,scheduler.next(),client.counts(),scheduler.paused());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}
void start_tesla(const Profile& p) {
    if(xTaskCreate(worker,"tesla_worker",65536,const_cast<Profile*>(&p),3,nullptr)!=pdPASS)fail("tesla_task");
}
}
