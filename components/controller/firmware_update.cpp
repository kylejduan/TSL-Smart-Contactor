#include "firmware_update.hpp"
#include <cstring>
namespace tsl {
const char* update_state_name(UpdateState s) {
    switch(s) {
    case UpdateState::Idle:return "idle";
    case UpdateState::Preparing:return "preparing";
    case UpdateState::Receiving:return "receiving";
    case UpdateState::Writing:return "writing";
    case UpdateState::Verifying:return "verifying";
    case UpdateState::Rebooting:return "rebooting";
    case UpdateState::Failed:return "failed";
    }
    return "failed";
}
bool compatible_firmware(const uint8_t* p,size_t n) {
    if(!p || n<288 || p[0]!=0xe9 || !p[1] || p[1]>16 || p[12]!=9 || p[13]!=0 ||
       p[32]!=0x32 || p[33]!=0x54 || p[34]!=0xcd || p[35]!=0xab)return false;
    constexpr char project[]="tsl_smart_contactor";
    if(std::memcmp(p+80,project,sizeof project))return false;
    // Signing a future version also asserts NVS/layout backward compatibility.
    const auto* v=reinterpret_cast<const char*>(p+48);
    if(v[0]!='0' || v[1]!='.' || !std::memchr(v,0,32))return false;
    unsigned minor=0;size_t i=2;
    if(v[i]<'0' || v[i]>'9')return false;
    for(;i<8 && v[i]>='0' && v[i]<='9';++i)minor=minor*10+unsigned(v[i]-'0');
    if(minor<2 || v[i++]!='.' || v[i]<'0' || v[i]>'9')return false;
    for(;i<32 && v[i]>='0' && v[i]<='9';++i){}
    return i<32 && v[i]==0;
}
bool FirmwareUpdate::active() const {return state_!=UpdateState::Idle && state_!=UpdateState::Failed;}
bool FirmwareUpdate::current(Ms now,uint32_t gen) const {
    return active() && gen==generation_ && now>=progress_ && now>=started_ &&
        now-started_<kFirmwareTotalMs && now-progress_<kFirmwareIdleMs;
}
bool FirmwareUpdate::begin(size_t bytes,uint32_t gen,Ms now) {
    if(active() || !gen || bytes<8192 || bytes>kFirmwareSlotBytes || bytes%4096)return false;
    total_=bytes;received_=queued_=0;generation_=gen;started_=progress_=now;
    error_="none";state_=UpdateState::Preparing;return true;
}
bool FirmwareUpdate::ready(Ms now,uint32_t gen) {
    if(state_!=UpdateState::Preparing || !current(now,gen))return false;
    state_=UpdateState::Receiving;progress_=now;return true;
}
bool FirmwareUpdate::chunk(size_t offset,const uint8_t* data,size_t n,Ms now,uint32_t gen) {
    if(state_!=UpdateState::Receiving || !current(now,gen) || offset!=received_ || !data ||
       !n || n>kFirmwareChunkBytes || n>total_-received_ ||
       (!received_ && !compatible_firmware(data,n)))return false;
    queued_=n;state_=UpdateState::Writing;return true;
}
bool FirmwareUpdate::written(size_t n,Ms now,uint32_t gen) {
    if(state_!=UpdateState::Writing || n!=queued_ || !current(now,gen))return false;
    received_+=n;queued_=0;state_=UpdateState::Receiving;progress_=now;return true;
}
bool FirmwareUpdate::finish(Ms now,uint32_t gen) {
    if(state_!=UpdateState::Receiving || received_!=total_ || !current(now,gen))return false;
    state_=UpdateState::Verifying;return true;
}
bool FirmwareUpdate::verified(Ms now,uint32_t gen) {
    if(state_!=UpdateState::Verifying || !current(now,gen))return false;
    state_=UpdateState::Rebooting;progress_=now;return true;
}
void FirmwareUpdate::fail(const char* error) {state_=UpdateState::Failed;error_=error;queued_=0;}
}
