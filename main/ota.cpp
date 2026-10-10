#include "ota.hpp"
#include "firmware_update.hpp"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include <cstdio>
#include <cstring>
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE || !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
#error "Production OTA requires rollback and signed RSA updates"
#endif
#if CONFIG_SECURE_BOOT || CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK || CONFIG_SECURE_FLASH_ENC_ENABLED
#error "This project must not enable irreversible eFuse security features"
#endif
namespace app {
std::atomic<bool> firmware_busy{false},setup_complete{false};
namespace {
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
FirmwareUpdate transfer;
uint8_t buffer[kFirmwareChunkBytes]={};
size_t queued_bytes=0;
char upload_id[33]={};
bool available=false,boot_pending=false,boot_checked=false;
const char* boot_result="normal";
const esp_partition_t* running=nullptr;
const esp_partition_t* target=nullptr;
esp_ota_handle_t handle=0;
bool handle_open=false,boot_selected=false;
Ms accepted_boot_at=0,reboot_at=0;
FirmwareUpdate view() {
    portENTER_CRITICAL(&lock);auto copy=transfer;portEXIT_CRITICAL(&lock);return copy;
}
bool current() {
    auto s=snapshot();
    return !critical_fault && !provisioning && s.ready && !s.inhibited &&
        view().current(now_ms(),s.generation);
}
void failed(const char* reason) {
    // esp_ota_end always consumes its handle, even on validation failure.
    if(handle_open) {
        if(esp_ota_abort(handle)!=ESP_OK)fail("ota_abort");
        handle_open=false;
    }
    if(boot_selected) {
        // OFF/configuration changes between boot selection and reboot cancel it.
        if(esp_ota_set_boot_partition(running)!=ESP_OK)fail("ota_restore_boot");
        boot_selected=false;
    }
    portENTER_CRITICAL(&lock);transfer.fail(reason);portEXIT_CRITICAL(&lock);
    // On failure the current mode resumes, subject to a new continuous OFF dwell.
    firmware_busy=false;
}
void check_boot() {
    if(!boot_pending || boot_checked)return;
    auto s=snapshot();
    if(critical_fault || now_ms()>30000) {
        boot_checked=true;
        portENTER_CRITICAL(&lock);boot_result="rollback_requested";portEXIT_CRITICAL(&lock);
        if(esp_ota_mark_app_invalid_rollback_and_reboot()!=ESP_OK)fail("ota_rollback");
        return;
    }
    // Local health only: do not require internet, Fleet API or fresh GPS. The
    // control task has fed its own watchdog after publishing each GPIO command.
    if(!setup_complete || !s.ready || s.inhibited || provisioning ||
       s.decision.commanded || control_max_gap_ms>100)return;
    if(!accepted_boot_at)accepted_boot_at=now_ms();
    if(now_ms()-accepted_boot_at<5000)return;
    if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK) {
        fail("ota_boot_confirm");return;
    }
    boot_checked=true;
    portENTER_CRITICAL(&lock);boot_pending=false;boot_result="confirmed";portEXIT_CRITICAL(&lock);
    firmware_busy=false;
}
void worker(void*) {
    while(true) {
        check_boot();
        auto v=view();
        if(v.active()) {
            if(!current())failed("canceled_or_expired");
            else if(v.state()==UpdateState::Preparing) {
                auto s=snapshot();
                // Allow an in-flight refresh to finish storing its rotated token.
                // No new Fleet operation may begin while firmware_busy is true.
                if(!s.decision.commanded && !network_busy) {
                    portENTER_CRITICAL(&lock);
                    if(!transfer.ready(now_ms(),s.generation))transfer.fail("canceled_or_expired");
                    portEXIT_CRITICAL(&lock);
                }
            } else if(v.state()==UpdateState::Writing) {
                if(!handle_open) {
                    // Sequential erase avoids a multi-second full-slot erase.
                    if(esp_ota_begin(target,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK) {
                        failed("flash_begin_failed");continue;
                    }
                    handle_open=true;
                }
                if(!current()) {failed("canceled_or_expired");continue;}
                if(esp_ota_write(handle,buffer,queued_bytes)!=ESP_OK) {
                    failed("flash_write_failed");continue;
                }
                const auto s=snapshot();
                portENTER_CRITICAL(&lock);
                bool ok=transfer.written(queued_bytes,now_ms(),s.generation);
                portEXIT_CRITICAL(&lock);
                if(!ok || !current())failed("canceled_or_expired");
            } else if(v.state()==UpdateState::Verifying) {
                auto result=handle_open ? esp_ota_end(handle) : ESP_FAIL;
                handle_open=false;
                if(result!=ESP_OK) {failed("signature_or_image_invalid");continue;}
                if(!current()) {failed("canceled_or_expired");continue;}
                // A failed commit may still have written one otadata sector.
                // Attempt to restore the running partition even on that error.
                boot_selected=true;
                if(esp_ota_set_boot_partition(target)!=ESP_OK) {
                    failed("boot_selection_failed");continue;
                }
                const auto s=snapshot();
                portENTER_CRITICAL(&lock);
                bool ok=transfer.verified(now_ms(),s.generation);
                portEXIT_CRITICAL(&lock);
                if(!ok || !current()) {failed("canceled_or_expired");continue;}
                reboot_at=now_ms()+2000;
            } else if(v.state()==UpdateState::Rebooting && now_ms()>=reboot_at) {
                if(current())esp_restart();
                else failed("canceled_or_expired");
            }
        } else if(firmware_busy && !boot_pending && boot_checked)failed("upload_aborted");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}
void ota_boot_initialize() {
    running=esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if(running && esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY) {
        boot_pending=true;boot_result="pending_self_test";firmware_busy=true;
    } else boot_checked=true;
}
bool start_ota() {
    target=esp_ota_get_next_update_partition(nullptr);
    esp_image_sig_public_key_digests_t keys{};
    available=running && target && target!=running && target->size==kFirmwareSlotBytes &&
        running->size==kFirmwareSlotBytes &&
        running->subtype>=ESP_PARTITION_SUBTYPE_APP_OTA_0 && running->subtype<=ESP_PARTITION_SUBTYPE_APP_OTA_15 &&
        esp_secure_boot_get_signature_blocks_for_running_app(true,&keys)==ESP_OK && keys.num_digests==1;
    if(boot_pending && !available)fail("ota_boot_signature");
    if(!boot_pending && esp_ota_get_last_invalid_partition())boot_result="previous_update_failed";
    const bool created=xTaskCreate(worker,"firmware_update",6144,nullptr,2,nullptr)==pdPASS;
    if(!created && boot_pending) {
        // With no update worker, its startup timeout cannot run. Roll back here
        // rather than letting a healthy control task feed a pending boot forever.
        fail("ota_task");
        if(esp_ota_mark_app_invalid_rollback_and_reboot()!=ESP_OK)fail("ota_rollback");
    }
    return created;
}
bool ota_begin(size_t bytes,uint32_t expected) {
    auto s=snapshot();
    if(!available || firmware_busy || critical_fault || provisioning || !s.ready || s.inhibited ||
       s.decision.timed || expected!=s.generation ||
       (effective_outage_policy(s.config)==OutagePolicy::HoldLast &&
        (s.auto_saved_generation!=s.generation || s.auto_saved!=s.decision.auto_state)))return false;
    uint8_t random[16];esp_fill_random(random,sizeof random);
    portENTER_CRITICAL(&lock);
    bool ok=transfer.begin(bytes,expected,now_ms());
    if(ok) {
        for(size_t i=0;i<sizeof random;++i)std::snprintf(upload_id+2*i,3,"%02x",random[i]);
        firmware_busy=true;
    }
    portEXIT_CRITICAL(&lock);return ok;
}
bool ota_chunk(const char* id,size_t offset,const uint8_t* data,size_t bytes) {
    auto s=snapshot();
    portENTER_CRITICAL(&lock);
    bool ok=constant_equal(id,upload_id) && transfer.chunk(offset,data,bytes,now_ms(),s.generation);
    if(ok) {std::memcpy(buffer,data,bytes);queued_bytes=bytes;}
    portEXIT_CRITICAL(&lock);return ok;
}
bool ota_finish(const char* id) {
    auto s=snapshot();
    portENTER_CRITICAL(&lock);
    bool ok=constant_equal(id,upload_id) && transfer.finish(now_ms(),s.generation);
    portEXIT_CRITICAL(&lock);return ok;
}
bool ota_abort(const char* id) {
    portENTER_CRITICAL(&lock);
    bool ok=transfer.active() && constant_equal(id,upload_id);
    if(ok)transfer.fail("upload_aborted");
    portEXIT_CRITICAL(&lock);return ok;
}
size_t ota_status_json(char* out,size_t capacity) {
    char build[65]={};
    const auto* description=esp_app_get_description();
    for(size_t i=0;i<32;++i)std::snprintf(build+2*i,3,"%02x",description->app_elf_sha256[i]);
    portENTER_CRITICAL(&lock);
    const int n=std::snprintf(out,capacity,
        "{\"available\":%s,\"busy\":%s,\"state\":\"%s\",\"received\":%u,\"total\":%u,"
        "\"upload_id\":\"%s\",\"error\":\"%s\",\"boot_check\":\"%s\",\"slot\":\"%s\",\"version\":\"%s\",\"build_sha256\":\"%s\"}",
        available?"true":"false",firmware_busy?"true":"false",update_state_name(transfer.state()),
        unsigned(transfer.received()),unsigned(transfer.total()),transfer.active()?upload_id:"",
        transfer.error(),boot_result,running?running->label:"unknown",description->version,build);
    portEXIT_CRITICAL(&lock);
    return n>0 && size_t(n)<capacity ? size_t(n) : 0;
}
}
