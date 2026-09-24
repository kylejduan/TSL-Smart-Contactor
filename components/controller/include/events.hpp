#pragma once
#include "policy.hpp"
#include <array>
#include <cstddef>
namespace tsl {
struct Event { Ms at=0; Reason reason=Reason::NoAuthorization; bool commanded=false; };
// Control-task writer; snapshots copy this bounded RAM-only history.
class EventLog {
public:
    void record(Ms at,Reason reason,bool commanded) {
        events_[next_]={at,reason,commanded};next_=(next_+1)%events_.size();
        if(count_<events_.size())++count_;
    }
    size_t size() const {return count_;}
    const Event* newest(size_t index) const {
        if(index>=count_)return nullptr;
        return &events_[(next_+events_.size()-1-index)%events_.size()];
    }
private:
    std::array<Event,16> events_{};
    size_t next_=0,count_=0;
};
}
