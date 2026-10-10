#pragma once
#include "runtime.hpp"
namespace app {
extern std::atomic<bool> firmware_busy, setup_complete;
// Call after the GPIO's early OFF initialization. No flash writes here.
void ota_boot_initialize();
bool start_ota();
bool ota_begin(size_t bytes,uint32_t generation);
bool ota_chunk(const char* id,size_t offset,const uint8_t*,size_t);
bool ota_finish(const char* id);
bool ota_abort(const char* id);
size_t ota_status_json(char*,size_t);
}
