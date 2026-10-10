#include "runtime_sdk_stubs.hpp"
#include "runtime.hpp"
#include "storage.hpp"
#include "ota.hpp"
#include "auto_journal.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" void app_main();
namespace {
// Every scenario runs in its own process, just as a boot starts with new RAM.
// All identities/time are synthetic. SDK calls execute synchronously here;
// this does not simulate FreeRTOS concurrency, hardware or watchdog reset.
constexpr std::int64_t synthetic_utc = 1800000000;
struct EndSimulation {};
#define CHECK(value) do { if (!(value)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #value); } while (false)
struct Simulation {
    tsl::Ms now = 0, until = 30200;
    bool driver = false, gpio = false, missing_profile = false;
    bool latch_failure = false, setup_failure = false, psram_failure = false;
    bool ota_failure=false;
    bool usb_failure = false, storage_failure = false, monitor_failure = false, queue_failure = false;
    bool high_failure = false, watchdog_add_failure = false, watchdog_feed_failure = false;
    bool send_home = false, send_sleep = false, overflow = false;
    bool inside_setup = false;
    bool block_commit=false,fail_commit=false,auto_task_failure=false,corrupt_record=false;
    bool inspect_swap=false,swap_checked=false,inside_hook=false;
    esp_reset_reason_t reset=ESP_RST_POWERON;
    tsl::AutoRecord saved{};
    bool record_present=false;
    std::map<std::string,std::vector<uint8_t>> other_blobs;
    unsigned feeds = 0, gpio_calls = 0, published_gpio_calls = 0;
    AllocationCallback allocation_callback = nullptr;
    app::Profile profile{};
    std::deque<tsl::Observation> queue;
    std::vector<std::string> startup;
    std::vector<std::pair<tsl::Ms, bool>> edges;
    std::function<void(tsl::Ms)> advance;
} sim;
bool ever_on() {
    return std::any_of(sim.edges.begin(), sim.edges.end(), [](const auto& edge) { return edge.second; });
}
void expect_fault(const char* source) {
    CHECK(app::critical_fault);
    CHECK(app::fault_source.load() && std::strcmp(app::fault_source.load(), source) == 0);
    CHECK(!sim.gpio && !app::snapshot().decision.commanded);
}
void home() {
    tsl::Observation fix;
    fix.generation = app::snapshot().generation;
    fix.request = 1;
    std::strcpy(fix.vin, sim.profile.config.vin);
    fix.kind = tsl::Evidence::Location;
    fix.vehicle = tsl::Vehicle::Online;
    fix.source_s = synthetic_utc;
    CHECK(app::submit(fix));
}
}

void runtime_unlock_hook() {
    if(!sim.inspect_swap || sim.inside_hook)return;
    sim.inside_hook=true;
    const auto current=app::snapshot();
    if(current.config.home_lon==0.1 && !current.inhibited) {
        CHECK(current.decision.auto_state==tsl::AutoState::Unknown);
        CHECK(!current.decision.auto_home && current.decision.last_source_s==0);
        app::auto_storage_step();sim.inspect_swap=false;sim.swap_checked=true;
    }
    sim.inside_hook=false;
}

// Linker wrapping changes only this test executable's wall clock calls.
extern "C" std::time_t __wrap_time(std::time_t* destination) {
    const auto value = static_cast<std::time_t>(synthetic_utc + sim.now / 1000);
    if (destination) *destination = value;
    return value;
}
esp_err_t gpio_set_level(gpio_num_t pin, int level) {
    CHECK(pin == GPIO_NUM_47 && (level == 0 || level == 1));
    ++sim.gpio_calls;
    if (!sim.driver) {
        sim.startup.emplace_back(level ? "latch_on" : "latch_off");
        if (sim.latch_failure) return ESP_FAIL;
    }
    if (level && sim.high_failure) return ESP_FAIL;
    if (sim.edges.empty() || sim.gpio != bool(level)) sim.edges.emplace_back(sim.now, bool(level));
    sim.gpio = bool(level);
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, int mode) {
    CHECK(pin == GPIO_NUM_47 && mode == GPIO_MODE_OUTPUT);
    CHECK(!sim.gpio);
    sim.startup.emplace_back("output_driver");
    sim.driver = true;
    return ESP_OK;
}
void vTaskPrioritySet(TaskHandle_t, UBaseType_t priority) { CHECK(priority == 10); }
BaseType_t xTaskCreate(TaskFunction_t, const char* name, std::uint32_t, void*, UBaseType_t, TaskHandle_t*) {
    if(std::strcmp(name,"auto_storage")==0)return sim.auto_task_failure ? pdFALSE : pdPASS;
    CHECK(std::strcmp(name, "usb_provision") == 0);
    return sim.usb_failure ? pdFALSE : pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char* name, std::uint32_t, void* argument,
                                 UBaseType_t, TaskHandle_t*, BaseType_t core) {
    CHECK(std::strcmp(name, "setup") == 0 && core == 0);
    sim.startup.emplace_back("setup_task");
    if (sim.setup_failure) return pdFALSE;
    sim.inside_setup = true;
    task(argument);
    sim.inside_setup = false;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t) { CHECK(sim.inside_setup); }
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(sim.now); }
void vTaskDelayUntil(TickType_t* wake, TickType_t interval) {
    CHECK(interval == 50 && !sim.inside_setup);
    CHECK(app::snapshot().decision.commanded == sim.gpio);
    *wake += interval;
    sim.now += interval;
    if (sim.advance) sim.advance(sim.now);
    if(!sim.block_commit)app::auto_storage_step();
    if (sim.now >= sim.until) throw EndSimulation{};
}
void vTaskDelay(TickType_t) {throw std::runtime_error("unexpected storage thread execution");}
QueueHandle_t xQueueCreateStatic(UBaseType_t count, UBaseType_t size, std::uint8_t*, StaticQueue_t*) {
    CHECK(count == 8 && size == sizeof(tsl::Observation));
    return sim.queue_failure ? nullptr : &sim.queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void* data, TickType_t timeout) {
    CHECK(queue == &sim.queue && timeout == 0);
    if (sim.queue.size() == 8) return pdFALSE;
    sim.queue.push_back(*static_cast<const tsl::Observation*>(data));
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void* data, TickType_t timeout) {
    CHECK(queue == &sim.queue && timeout == 0);
    if (sim.queue.empty()) return pdFALSE;
    *static_cast<tsl::Observation*>(data) = sim.queue.front();
    sim.queue.pop_front();
    return pdTRUE;
}
std::int64_t esp_timer_get_time() { return sim.now * 1000; }
esp_err_t esp_task_wdt_add(TaskHandle_t) { return sim.watchdog_add_failure ? ESP_FAIL : ESP_OK; }
esp_err_t esp_task_wdt_reset() {
    CHECK(!sim.inside_setup);
    // Verify the actual entrypoint publishes GPIO/state before feeding, and a
    // worker does not manufacture independent feeds during blocked networking.
    CHECK(sim.gpio_calls > sim.published_gpio_calls);
    CHECK(app::snapshot().decision.commanded == sim.gpio);
    sim.published_gpio_calls = sim.gpio_calls;
    ++sim.feeds;
    return sim.watchdog_feed_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t heap_caps_register_failed_alloc_callback(AllocationCallback callback) {
    sim.allocation_callback = callback;
    return sim.monitor_failure ? ESP_FAIL : ESP_OK;
}
bool esp_psram_is_initialized() { return !sim.psram_failure; }
std::size_t esp_psram_get_size() { return 8 * 1024 * 1024; }
std::uint32_t esp_random() { return 12345; }
esp_reset_reason_t esp_reset_reason() {return sim.reset;}
namespace app {
std::atomic<bool> firmware_busy{false},setup_complete{false};
void ota_boot_initialize() {}
bool start_ota() {return !sim.ota_failure;}
NvsStore& storage() { static NvsStore store; return store; }
bool NvsStore::initialize() { sim.startup.emplace_back("storage"); return !sim.storage_failure; }
ReadResult NvsStore::read(const char* key, void* out, std::size_t size) {
    if(std::strcmp(key,"auto_state")) {
        const auto found=sim.other_blobs.find(key);
        if(found==sim.other_blobs.end())return ReadResult::Missing;
        CHECK(found->second.size()==size);std::memcpy(out,found->second.data(),size);return ReadResult::Ok;
    }
    CHECK(std::strcmp(key,"auto_state")==0 && size==sizeof sim.saved);
    if(!sim.record_present)return ReadResult::Missing;
    std::memcpy(out,&sim.saved,size);return ReadResult::Ok;
}
bool NvsStore::write(const char* key, const void* input, std::size_t size) {
    if(std::strcmp(key,"auto_state")) {
        const auto* data=static_cast<const uint8_t*>(input);sim.other_blobs[key]={data,data+size};return true;
    }
    CHECK(std::strcmp(key,"auto_state")==0 && size==sizeof sim.saved);
    if(sim.fail_commit)return false;
    std::memcpy(&sim.saved,input,size);sim.record_present=true;return true;
}
Error load_auto_state(const Config& config,AutoState& state,int64_t& source,bool resume) {
    return recover_auto_state(storage(),config,resume,state,source);
}
Error persist_auto_state(const Snapshot& requested) {
    const auto current=snapshot();
    if(current.generation!=requested.generation || provisioning)return Error::Unavailable;
    AutoJournal journal(storage());
    return critical_fault || current.inhibited || current.config.disabled ? journal.clear() :
        journal.save(current.config,requested.decision.auto_state,requested.decision.last_source_s);
}
ReadResult load_profile(Profile& profile) {
    profile = sim.profile;
    return sim.missing_profile ? ReadResult::Missing : ReadResult::Ok;
}
bool valid_profile(const Profile& profile) { return valid_config(profile.config); }
void usb_task(void*) { throw std::runtime_error("unexpected USB task execution"); }
void start_wifi(const Profile&) {
    wifi_connected = true;
    utc_synced = true;
    utc_continuity = true;
    utc_last_sync_uptime_s = 0;
}
bool start_management(Profile&) { return true; }
void start_tesla(const Profile&) {
    if (sim.send_home) home();
    if (sim.send_sleep) {
        tsl::Observation asleep;
        asleep.generation = snapshot().generation;
        asleep.request = 1;
        std::strcpy(asleep.vin, sim.profile.config.vin);
        asleep.kind = tsl::Evidence::Asleep;
        asleep.vehicle = tsl::Vehicle::Asleep;
        CHECK(submit(asleep));
    }
    if (sim.overflow) {
        for (unsigned i = 0; i < 8; ++i) CHECK(submit(tsl::Observation{}));
        CHECK(!submit(tsl::Observation{}));
    }
}
}

int main(int argc, char** argv) {
    try {
        CHECK(argc == 2);
        const std::string scenario = argv[1];
        auto& cfg = sim.profile.config;
        std::strcpy(cfg.vin, "5YJ3E1EA7KF000001");
        cfg.commissioned = true;
        cfg.disabled = false;
        cfg.dry_run = false;
        tsl::TokenJournal tokens(app::storage());CHECK(tokens.provision("synthetic-refresh-token")==tsl::Error::None);
        if(scenario.rfind("held_",0)==0)cfg.outage_policy=uint8_t(tsl::OutagePolicy::HoldLast);
        if(scenario.rfind("held_saved_",0)==0) {
            sim.saved.config_crc=tsl::auto_config_crc(cfg);
            sim.saved.state=scenario=="held_saved_away" ? tsl::AutoState::Away : tsl::AutoState::Home;
            tsl::seal(sim.saved);sim.record_present=true;
            if(scenario=="held_saved_disabled")cfg.disabled=true;
            if(scenario=="held_saved_dry_run") {cfg.dry_run=true;sim.saved.config_crc=tsl::auto_config_crc(cfg);tsl::seal(sim.saved);}
            if(scenario=="held_saved_watchdog")sim.reset=ESP_RST_TASK_WDT;
            if(scenario=="held_saved_corrupt")sim.saved.crc^=1;
        }
        if (scenario == "boot") sim.missing_profile = true;
        else if(scenario.rfind("held_saved_",0)==0) {
            sim.advance=[](tsl::Ms) {app::wifi_connected=false;app::utc_synced=false;app::utc_continuity=false;};
        }
        else if(scenario=="held_storage_task_failure")sim.auto_task_failure=true;
        else if(scenario=="held_commit_blocked" || scenario=="held_commit_failure" || scenario=="held_off_pending") {
            sim.send_home=true;sim.block_commit=true;
            if(scenario=="held_commit_failure") {sim.block_commit=false;sim.fail_commit=true;}
            if(scenario=="held_off_pending")sim.advance=[](tsl::Ms now) {
                if(now==30050) {CHECK(!sim.gpio);app::inhibit();sim.block_commit=false;}
            };
        }
        else if(scenario.rfind("ota_",0)==0) {
            sim.send_home=true;sim.until=80200;
            sim.advance=[&](tsl::Ms now) {
                if(now==40000) {CHECK(sim.gpio);app::firmware_busy=true;}
                if(now==40100)CHECK(!sim.gpio && app::snapshot().decision.auto_home);
                if(now==45000 && scenario=="ota_user_off")app::inhibit();
                if(now==50000)app::firmware_busy=false;
                if(now==69950)CHECK(!sim.gpio);
                if(now==70100)CHECK(sim.gpio==(scenario!="ota_user_off"));
            };
        }
        else if (scenario == "armed_boot" || scenario == "held_boot") {} // No reusable HOME state.
        else if (scenario == "asleep_boot" || scenario == "held_asleep_boot") sim.send_sleep = true;
        else if (scenario == "disabled") { cfg.disabled = true; sim.send_home = true; }
        else if (scenario == "uncommissioned") { cfg.commissioned = false; sim.send_home = true; }
        else if (scenario == "dry_run") { cfg.dry_run = true; sim.send_home = true; }
        else if (scenario == "latch_failure") sim.latch_failure = true;
        else if (scenario == "setup_failure") sim.setup_failure = true;
        else if (scenario == "usb_task_failure") sim.usb_failure = true;
        else if (scenario == "firmware_task_failure")sim.ota_failure=true;
        else if (scenario == "storage_failure") sim.storage_failure = true;
        else if (scenario == "allocation_monitor_failure") sim.monitor_failure = true;
        else if (scenario == "queue_creation_failure") sim.queue_failure = true;
        else if (scenario == "psram_failure") sim.psram_failure = true;
        else if (scenario == "watchdog_add_failure") sim.watchdog_add_failure = true;
        else if (scenario == "queue_overflow") sim.overflow = true;
        else {
            sim.send_home = true;
            app::network_busy = true; // The worker stays blocked for the entire run.
            if (scenario == "lease_expiry" || scenario == "held_outage") sim.until = 900100;
            else if (scenario == "gpio_failure") sim.high_failure = true;
            else if (scenario == "off_during_io" || scenario == "held_off") {
                sim.advance = [&](tsl::Ms now) {
                    if (now != 30050) return;
                    CHECK(sim.gpio);
                    const auto committed = app::snapshot();
                    const auto before = committed.generation;
                    app::inhibit();
                    app::auto_commit_ack(committed); // Late storage completion cannot undo OFF.
                    CHECK(app::snapshot().auto_saved_generation!=app::snapshot().generation);
                    CHECK(!app::configure(cfg, before));
                    CHECK(!app::timed(3600, before));
                    // A result from the previously blocked request may arrive.
                    tsl::Observation late;
                    late.generation = before;
                    late.request = 2;
                    std::strcpy(late.vin, cfg.vin);
                    late.kind = tsl::Evidence::Location;
                    late.source_s = synthetic_utc + 30;
                    CHECK(app::submit(late));
                };
            } else if (scenario == "control_deadline" || scenario == "held_control_deadline") {
                sim.advance = [](tsl::Ms now) { if (now == 30050) sim.now += 300; };
                sim.until = 30500;
            } else if(scenario=="held_config_swap") {
                sim.advance=[&](tsl::Ms now) {
                    if(now!=30050)return;
                    CHECK(sim.gpio);uint32_t epoch;
                    CHECK(app::inhibit_current(app::snapshot().generation,epoch));
                    cfg.home_lon=0.1;
                    tsl::AutoJournal journal(app::storage());CHECK(journal.clear()==tsl::Error::None);
                    sim.inspect_swap=true;CHECK(app::configure(cfg,epoch));
                };
            } else if (scenario == "watchdog_feed_failure") {
                sim.advance = [](tsl::Ms now) { if (now == 30050) sim.watchdog_feed_failure = true; };
            } else if (scenario == "recoverable_allocation") {
                sim.advance = [](tsl::Ms now) {
                    if (now == 30050) {
                        CHECK(sim.allocation_callback);
                        sim.allocation_callback(8192, 0, "synthetic_network_allocation");
                    }
                };
            } else throw std::runtime_error("unknown scenario");
        }
        try { app_main(); CHECK(false); } catch (const EndSimulation&) {}
        CHECK(sim.feeds > 0);
        CHECK(sim.startup.front() == "latch_off");
        if (scenario != "latch_failure") CHECK(sim.startup[1] == "output_driver");
        if (scenario == "latch_failure") { expect_fault("gpio_init"); CHECK(!sim.driver); }
        else if(scenario.rfind("ota_",0)==0) {
            CHECK(ever_on());
            CHECK(app::snapshot().decision.commanded==(scenario!="ota_user_off"));
            CHECK(app::snapshot().config.disabled==(scenario=="ota_user_off"));
            CHECK(!app::critical_fault);
        }
        else if (scenario == "setup_failure") expect_fault("setup_or_usb_task");
        else if (scenario == "usb_task_failure") expect_fault("setup_or_usb_task");
        else if (scenario == "firmware_task_failure")expect_fault("ota_task");
        else if (scenario == "storage_failure") expect_fault("storage_init");
        else if (scenario == "allocation_monitor_failure") expect_fault("allocation_monitor");
        else if (scenario == "queue_creation_failure") expect_fault("setup_or_usb_task");
        else if (scenario == "psram_failure") expect_fault("psram_init");
        else if (scenario == "watchdog_add_failure") expect_fault("watchdog_init");
        else if (scenario == "queue_overflow") expect_fault("observation_queue");
        else if(scenario=="held_saved_corrupt")expect_fault("auto_state_load");
        else if(scenario=="held_commit_failure")expect_fault("auto_state_write");
        else if(scenario=="held_storage_task_failure")expect_fault("auto_storage_task");
        else if (scenario == "gpio_failure") expect_fault("gpio_command");
        else if (scenario == "control_deadline" || scenario == "held_control_deadline") { expect_fault("control_deadline"); CHECK(ever_on()); }
        else if (scenario == "watchdog_feed_failure") { expect_fault("watchdog_feed"); CHECK(ever_on()); }
        else {
            CHECK(!app::critical_fault);
            if(scenario=="held_saved_home") {
                CHECK(sim.gpio && app::snapshot().decision.restored && app::snapshot().decision.retained);
                CHECK(sim.edges[1]==std::make_pair(tsl::Ms(30000),true));
                CHECK(app::snapshot().decision.last_source_s==0 && !app::snapshot().utc_ok);
            } else if(scenario=="held_commit_blocked") {
                CHECK(!ever_on() && app::snapshot().decision.reason==tsl::Reason::HomePending);
                CHECK(app::snapshot().decision.auto_home && !sim.record_present);
            } else if(scenario=="held_off_pending") {
                CHECK(!ever_on() && app::snapshot().config.disabled);
                CHECK(sim.saved.state==tsl::AutoState::Unknown);
            } else if(scenario=="held_config_swap") {
                CHECK(sim.swap_checked && ever_on() && !sim.gpio);
                CHECK(app::snapshot().config.home_lon==0.1 && !app::snapshot().decision.auto_home);
                CHECK(sim.saved.state==tsl::AutoState::Unknown);
            } else if (scenario == "lease_expiry") {
                CHECK(sim.edges.size() == 3);
                CHECK(sim.edges[1] == std::make_pair(tsl::Ms(30000), true));
                CHECK(sim.edges[2] == std::make_pair(tsl::Ms(900000), false));
            } else if (scenario == "held_outage") {
                CHECK(sim.gpio && app::snapshot().decision.retained);
                CHECK(app::snapshot().decision.lease_left==0 && sim.edges.size()==2);
            } else if (scenario == "off_during_io" || scenario == "held_off") {
                CHECK(sim.edges.size() == 3);
                CHECK(sim.edges[2] == std::make_pair(tsl::Ms(30050), false));
                CHECK(app::snapshot().config.disabled && !app::snapshot().decision.auto_home);
            } else if (scenario == "recoverable_allocation") {
                CHECK(sim.gpio && app::failed_allocation_bytes == 8192);
            } else {
                CHECK(!ever_on());
                if (scenario == "dry_run") CHECK(app::snapshot().decision.desired);
                if(scenario=="held_saved_dry_run")CHECK(app::snapshot().decision.desired && app::snapshot().decision.restored);
                if(scenario=="held_saved_watchdog")CHECK(sim.saved.state==tsl::AutoState::Unknown);
            }
        }
        if (app::critical_fault && scenario != "control_deadline" && scenario != "held_control_deadline" && scenario != "watchdog_feed_failure")
            CHECK(!ever_on());
        std::cout << "PASS production app_main: " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
