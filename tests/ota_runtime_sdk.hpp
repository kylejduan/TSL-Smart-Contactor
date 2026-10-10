#pragma once
#include "runtime_sdk_stubs.hpp"
inline constexpr int ESP_PARTITION_SUBTYPE_APP_OTA_0=16,ESP_PARTITION_SUBTYPE_APP_OTA_15=31;
struct esp_partition_t {uint32_t address,size;int subtype;char label[17];};
enum esp_ota_img_states_t {ESP_OTA_IMG_VALID,ESP_OTA_IMG_PENDING_VERIFY};
using esp_ota_handle_t=uint32_t;
inline constexpr size_t OTA_WITH_SEQUENTIAL_WRITES=~size_t(1);
struct esp_image_sig_public_key_digests_t {unsigned num_digests=0;};
struct esp_app_desc_t {char version[32];uint8_t app_elf_sha256[32]={};};
const esp_app_desc_t* esp_app_get_description();
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_last_invalid_partition();
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
esp_err_t esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t*);
esp_err_t esp_secure_boot_get_signature_blocks_for_running_app(bool,esp_image_sig_public_key_digests_t*);
esp_err_t esp_ota_begin(const esp_partition_t*,size_t,esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t,const void*,size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();
void esp_restart();
void esp_fill_random(void*,size_t);
