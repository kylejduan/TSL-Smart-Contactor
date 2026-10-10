#include "storage.hpp"
namespace app {
void auto_storage_step() {
    const auto s=snapshot();
    if(!s.ready || provisioning || effective_outage_policy(s.config)!=OutagePolicy::HoldLast)return;
    if(!critical_fault && !s.inhibited && s.auto_saved_generation==s.generation &&
       s.auto_saved==s.decision.auto_state)return;
    auto result=persist_auto_state(s);
    if(result==Error::None)auto_commit_ack(s);
    else if(result!=Error::Unavailable)fail("auto_state_write");
}
static void auto_storage_task(void*) {
    while(true) {auto_storage_step();vTaskDelay(pdMS_TO_TICKS(100));}
}
bool start_auto_storage() {
    return xTaskCreate(auto_storage_task,"auto_storage",6144,nullptr,2,nullptr)==pdPASS;
}
}
