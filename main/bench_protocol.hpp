#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// This code is included only by the separately selected bench application and
// its native tests. It has no production controller, network or storage hooks.
namespace bench {
using Ms = std::uint64_t;
inline constexpr Ms off_dwell_ms = 30000;
inline constexpr Ms pulse_ms = 2000;
inline constexpr Ms frame_timeout_ms = 1000;
enum class Reply { None, Status, Started, Off, Malformed, WrongNonce, Dwell, Used, Fault };
inline const char* reply_name(Reply reply) {
    switch(reply) {
    case Reply::Status:return "status";
    case Reply::Started:return "started";
    case Reply::Off:return "off";
    case Reply::Malformed:return "malformed_command";
    case Reply::WrongNonce:return "wrong_nonce";
    case Reply::Dwell:return "off_dwell";
    case Reply::Used:return "one_shot_used";
    case Reply::Fault:return "fault";
    default:return "none";
    }
}
inline bool accepted(Reply reply) {
    return reply==Reply::Status || reply==Reply::Started || reply==Reply::Off;
}
class Session {
public:
    Session(Ms boot, std::string_view nonce):boot_(boot),last_(boot) {
        if(nonce.size()!=32) {fail();return;}
        for(char c:nonce)if(!((c>='0' && c<='9') || (c>='a' && c<='f'))) {fail();return;}
        std::memcpy(nonce_.data(),nonce.data(),32);
    }
    void fail() {fault_=true;on_=false;used_=true;}
    void tick(Ms now) {
        if(now<last_)fail();
        last_=now;
        if(on_ && now-pulse_start_>=pulse_ms)on_=false;
        if(length_ && (now<frame_start_ || now-frame_start_>=frame_timeout_ms)) {
            length_=0;discard_=true;
        }
    }
    Reply receive(char byte, Ms now) {
        tick(now);
        if(byte=='\n') {
            Reply result=Reply::Malformed;
            if(!discard_) {
                if(length_ && line_[length_-1]=='\r')--length_;
                result=command(std::string_view(line_.data(),length_),now);
            }
            length_=0;discard_=false;
            return result;
        }
        if(discard_)return Reply::None;
        if(!length_)frame_start_=now;
        const auto value=static_cast<unsigned char>(byte);
        if((value<32 && byte!='\r') || value>126 || length_==line_.size()) {
            length_=0;discard_=true;
        } else line_[length_++]=byte;
        return Reply::None;
    }
    bool on() const {return on_;}
    bool used() const {return used_;}
    bool fault() const {return fault_;}
    const char* nonce() const {return nonce_.data();}
    Ms dwell_remaining(Ms now) const {
        if(now<boot_)return off_dwell_ms;
        const Ms elapsed=now-boot_;
        return elapsed<off_dwell_ms ? off_dwell_ms-elapsed : 0;
    }
private:
    Reply command(std::string_view line, Ms now) {
        if(line=="OFF") {on_=false;used_=true;return Reply::Off;}
        if(line=="STATUS")return Reply::Status;
        if(line.size()!=38 || line.substr(0,6)!="START ")return Reply::Malformed;
        if(fault_)return Reply::Fault;
        if(line.substr(6)!=std::string_view(nonce_.data(),32))return Reply::WrongNonce;
        if(used_)return Reply::Used;
        if(dwell_remaining(now))return Reply::Dwell;
        // Consume before the adapter can command HIGH. No reset/re-arm command.
        used_=true;pulse_start_=now;on_=true;
        return Reply::Started;
    }
    std::array<char,33> nonce_{};
    std::array<char,48> line_{};
    std::size_t length_=0;
    Ms boot_,last_,pulse_start_=0,frame_start_=0;
    bool on_=false,used_=false,fault_=false,discard_=false;
};
}
