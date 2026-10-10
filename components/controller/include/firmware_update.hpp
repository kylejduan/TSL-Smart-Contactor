#pragma once
#include "policy.hpp"
#include <cstddef>
#include <cstdint>
namespace tsl {
constexpr size_t kFirmwareSlotBytes=0x300000, kFirmwareChunkBytes=4096;
constexpr Ms kFirmwareTotalMs=180000, kFirmwareIdleMs=30000;
enum class UpdateState : uint8_t { Idle, Preparing, Receiving, Writing, Verifying, Rebooting, Failed };
const char* update_state_name(UpdateState);
// Compatibility only; SDK esp_ota_end verifies the complete signed image.
bool compatible_firmware(const uint8_t*,size_t);
class FirmwareUpdate {
public:
    bool begin(size_t bytes,uint32_t generation,Ms now);
    bool ready(Ms now,uint32_t generation);
    bool chunk(size_t offset,const uint8_t*,size_t,Ms now,uint32_t generation);
    bool written(size_t bytes,Ms now,uint32_t generation);
    bool finish(Ms now,uint32_t generation);
    bool verified(Ms now,uint32_t generation);
    bool current(Ms now,uint32_t generation) const;
    bool active() const;
    void fail(const char* static_error);
    UpdateState state() const {return state_;}
    size_t received() const {return received_;}
    size_t total() const {return total_;}
    const char* error() const {return error_;}
private:
    UpdateState state_=UpdateState::Idle;
    size_t received_=0,total_=0,queued_=0;
    uint32_t generation_=0;
    Ms started_=0,progress_=0;
    const char* error_="none";
};
}
