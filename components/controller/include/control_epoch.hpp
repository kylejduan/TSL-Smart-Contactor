#pragma once
#include <cstdint>

namespace tsl {
// Serialize access with the control lock. A command carries the generation it
// observed before authentication/transport delays. OFF invalidates every older
// command; only a fresh explicit command may start another change afterward.
class ControlEpoch {
public:
    explicit ControlEpoch(uint32_t initial=1) : value_(initial ? initial : 1) {}
    uint32_t current() const { return value_; }
    bool matches(uint32_t expected) const { return expected!=0 && expected==value_; }
    uint32_t cancel() {
        if(++value_==0)++value_;
        return value_;
    }
    bool begin(uint32_t expected,uint32_t& acquired) {
        if(!matches(expected))return false;
        acquired=cancel();return true;
    }
private:
    uint32_t value_=1;
};
}
