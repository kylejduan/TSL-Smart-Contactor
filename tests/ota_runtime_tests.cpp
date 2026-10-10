// Execute the real SDK OTA adapter with synthetic flash/time/control calls.
#include "ota_runtime_sdk.hpp"
#include "ota.hpp"
#include "firmware_update.hpp"
#include "json.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#define CHECK(v) do {if(!(v))throw std::runtime_error(#v);}while(false)
namespace {
struct Stop {};struct Restart {};
std::string scenario;
tsl::Ms now=0,until=6000;
app::Snapshot state;
TaskFunction_t worker=nullptr;
esp_partition_t a{0x30000,0x300000,16,"ota_0"},b{0x330000,0x300000,17,"ota_1"};
const esp_partition_t* selected=&a;
unsigned begins=0,writes=0,ends=0,aborts=0,restarts=0,confirms=0,rollbacks=0;
bool flash_open=false,started=false,finished=false,hook=false;
std::array<uint8_t,4096> chunk{};
char id[33]={};
std::string update_state() {
    char out[512];CHECK(app::ota_status_json(out,sizeof out));tsl::Json j;CHECK(j.parse(out));
    char result[32];CHECK(j.string(j.get(0,"state"),result,sizeof result));
    j.string(j.get(0,"upload_id"),id,sizeof id);return result;
}
void advance() {
    now+=50;
    // Fake control tick applies the maintenance gate, never a network worker.
    state.decision.commanded=false;
    if(scenario.rfind("boot_",0)==0) {
        if(scenario=="boot_fault" && now>=200)app::critical_fault=true;
        if(now>=until)throw Stop{};
        return;
    }
    if(!started) {
        CHECK(app::ota_begin(8192,state.generation));started=true;
        if(scenario=="timeout")now=31000;
    } else if(!finished) {
        auto s=update_state();
        if(s=="receiving") {
            char out[512];CHECK(app::ota_status_json(out,sizeof out));tsl::Json j;CHECK(j.parse(out));
            int64_t bytes;CHECK(j.integer(j.get(0,"received"),bytes));
            if(bytes==8192) {CHECK(app::ota_finish(id));finished=true;}
            else CHECK(app::ota_chunk(id,size_t(bytes),chunk.data(),chunk.size()));
        }
    }
    if(scenario=="off_after_select" && selected==&b && !hook) {
        ++state.generation;state.config.disabled=true;hook=true;
    }
    if(now>=until)throw Stop{};
}
}
namespace app {
std::atomic<bool> critical_fault{false},wifi_connected{false},utc_synced{false},utc_continuity{false},provisioning{false},network_busy{false};
std::atomic<uint32_t> control_max_gap_ms{50};
Snapshot snapshot() {return state;}
Ms now_ms() {return now;}
void fail(const char*) {critical_fault=true;}
}
void runtime_unlock_hook() {}
const esp_app_desc_t* esp_app_get_description() {static esp_app_desc_t d{"0.2.0"};return &d;}
const esp_partition_t* esp_ota_get_running_partition() {return &a;}
const esp_partition_t* esp_ota_get_last_invalid_partition() {return nullptr;}
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {return &b;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p,esp_ota_img_states_t* s) {
    CHECK(p==&a);*s=scenario.rfind("boot_",0)==0 ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID;return ESP_OK;
}
esp_err_t esp_secure_boot_get_signature_blocks_for_running_app(bool hash,esp_image_sig_public_key_digests_t* keys) {
    CHECK(hash);keys->num_digests=1;return ESP_OK;
}
BaseType_t xTaskCreate(TaskFunction_t task,const char* name,uint32_t,void*,UBaseType_t,TaskHandle_t*) {
    CHECK(std::strcmp(name,"firmware_update")==0);worker=task;return scenario=="boot_task_failure" ? pdFALSE : pdPASS;
}
void vTaskDelay(TickType_t ticks) {CHECK(ticks==50);advance();}
void esp_fill_random(void* dst,size_t n) {std::memset(dst,0xa5,n);}
esp_err_t esp_ota_begin(const esp_partition_t* p,size_t mode,esp_ota_handle_t* handle) {
    CHECK(p==&b && selected==&a && mode==OTA_WITH_SEQUENTIAL_WRITES);
    CHECK(app::firmware_busy && !state.decision.commanded && !app::network_busy);
    ++begins;if(scenario=="begin_failure")return ESP_FAIL;
    flash_open=true;*handle=9;return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void*,size_t bytes) {
    CHECK(h==9 && flash_open && bytes==4096);++writes;
    if(scenario=="off_during_write") {++state.generation;state.config.disabled=true;}
    return scenario=="write_failure" ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) {
    CHECK(h==9 && flash_open);flash_open=false;++ends;
    return scenario=="signature_failure" ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h) {CHECK(h==9 && flash_open);flash_open=false;++aborts;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p) {
    CHECK(!state.decision.commanded && app::firmware_busy);
    if(scenario=="select_failure" && p==&b) {selected=p;return ESP_FAIL;}
    selected=p;return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    CHECK(app::setup_complete && !app::critical_fault && !state.decision.commanded);
    ++confirms;return scenario=="boot_confirm_failure" ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {++rollbacks;throw Restart{};}
void esp_restart() {++restarts;CHECK(selected==&b && !state.decision.commanded);throw Restart{};}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);scenario=argv[1];state.ready=true;state.inhibited=false;state.generation=1;
        state.config.disabled=false;state.config.commissioned=true;state.config.outage_policy=1;
        state.decision.auto_state=tsl::AutoState::Home;state.decision.auto_home=true;
        state.auto_saved_generation=1;state.auto_saved=tsl::AutoState::Home;
        chunk[0]=0xe9;chunk[1]=4;chunk[12]=9;chunk[32]=0x32;chunk[33]=0x54;chunk[34]=0xcd;chunk[35]=0xab;
        std::strcpy(reinterpret_cast<char*>(chunk.data()+48),"0.2.1");
        std::strcpy(reinterpret_cast<char*>(chunk.data()+80),"tsl_smart_contactor");
        app::setup_complete=scenario!="boot_timeout";
        if(scenario=="boot_missing_profile")state.ready=false;
        if(scenario.rfind("boot_",0)==0)until=31000;
        if(scenario=="timeout")until=35000;
        app::ota_boot_initialize();
        try {CHECK(app::start_ota());worker(nullptr);}catch(const Stop&){}catch(const Restart&){}
        CHECK(!flash_open);
        if(scenario=="success") {CHECK(begins==1 && writes==2 && ends==1 && restarts==1 && selected==&b);}
        else if(scenario=="boot_success") {CHECK(confirms==1 && rollbacks==0 && !app::firmware_busy);}
        else if(scenario.rfind("boot_",0)==0) {CHECK(rollbacks==1 && app::firmware_busy);}
        else {CHECK(selected==&a && restarts==0 && !app::firmware_busy && update_state()=="failed");}
        if(scenario=="off_during_write" || scenario=="off_after_select")CHECK(state.config.disabled);
        else CHECK(!state.config.disabled && state.decision.auto_state==tsl::AutoState::Home);
        std::cout<<"PASS "<<scenario<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
