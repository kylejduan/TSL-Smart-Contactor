#include "reliability.hpp"
#include <algorithm>
namespace tsl {
Error TokenJournal::load() {
    auto r=store_.read("tokens",&record_,sizeof record_);
    if(r==ReadResult::Missing)return Error::Reauthorize;
    if(r!=ReadResult::Ok || !intact(record_) || record_.current[2048] || record_.previous[2048] ||
       !record_.current[0] || record_.attempts>3 || record_.pending>1 || record_.reauthorize>1)
        return Error::Storage;
    loaded_=true;
    return record_.reauthorize ? Error::Reauthorize : Error::None;
}
bool TokenJournal::save(TokenRecord& next) {
    seal(next);
    if(!store_.write("tokens",&next,sizeof next))return false;
    record_=next;loaded_=true;return true;
}
Error TokenJournal::provision(const char* t) {
    if(!t[0] || std::strlen(t)>2048 || busy_)return Error::Malformed;
    TokenRecord r;std::strcpy(r.current,t);
    return save(r) ? Error::None : Error::Storage;
}
Error TokenJournal::begin(int64_t utc) {
    if(!loaded_ || record_.reauthorize)return Error::Reauthorize;
    if(busy_)return Error::Authentication;
    if(utc<1704067200LL)return Error::Clock;
    if(record_.pending && (record_.attempts>=3 || utc<record_.uncertain_since ||
                          utc-record_.uncertain_since>=86400))return revoke();
    auto r=record_;
    if(!r.pending) {r.uncertain_since=utc;r.attempts=0;}
    r.pending=1;++r.attempts;
    if(!save(r))return Error::Storage;
    busy_=true;return Error::None;
}
Error TokenJournal::finish(const char* t) {
    if(!busy_ || !t[0] || std::strlen(t)>2048)return Error::Malformed;
    auto r=record_;
    std::strcpy(r.previous,r.current);std::strcpy(r.current,t);
    ++r.sequence;r.pending=0;r.attempts=0;r.uncertain_since=0;
    bool ok=save(r);busy_=false;
    return ok ? Error::None : Error::Storage;
}
Error TokenJournal::revoke() {
    auto r=record_;r.reauthorize=1;busy_=false;
    return save(r) ? Error::Reauthorize : Error::Storage;
}
Error Budget::load() {
    auto result=store_.read("budget",&record_,sizeof record_);
    if(result==ReadResult::Missing) {record_=BudgetRecord{};return Error::None;}
    return result==ReadResult::Ok && intact(record_) ? Error::None : Error::Storage;
}
Error Budget::take(Endpoint e,uint32_t day,uint32_t month,uint32_t dc,uint32_t mc) {
    const auto k=static_cast<size_t>(e);
    if(k>=3 || !day || !month || day<record_.day || month<record_.month)return Error::Clock;
    auto r=record_;
    if(day!=r.day || month!=r.month) {
        std::fill(std::begin(credit_),std::end(credit_),0);
        if(day!=r.day) {r.day=day;std::fill(std::begin(r.daily),std::end(r.daily),0);}
        if(month!=r.month) {r.month=month;std::fill(std::begin(r.monthly),std::end(r.monthly),0);}
    }
    uint64_t used_d=0,used_m=0;
    for(int i=0;i<3;++i) {used_d+=r.daily[i];used_m+=r.monthly[i];}
    if(used_d>dc || used_m>mc)return Error::Budget;
    if(credit_[k]) {--credit_[k];return Error::None;}
    if(used_d==dc || used_m==mc)return Error::Budget;
    uint32_t n=std::min<uint64_t>(4,std::min(dc-used_d,mc-used_m));
    r.daily[k]+=n;r.monthly[k]+=n;seal(r);
    if(!store_.write("budget",&r,sizeof r))return Error::Storage;
    record_=r;credit_[k]=n-1;return Error::None;
}
bool Scheduler::begin(Ms now) {
    if(!due(now))return false;
    busy_=true;return true;
}
void Scheduler::finish(Ms now,Error e,uint32_t poll,uint32_t retry,uint32_t random) {
    busy_=false;
    if(e==Error::None) {failures_=0;embargo_=0;next_=now+Ms(poll)*1000;return;}
    if(e==Error::Reauthorize || e==Error::Permission || e==Error::Billing ||
       e==Error::Redirect || e==Error::Storage || e==Error::Authentication)paused_=true;
    failures_=std::min(failures_+1,7u);
    uint32_t seconds=std::min(3600u,30u*(1u<<failures_));
    seconds+=random%31;
    seconds=std::max<uint32_t>(seconds,std::min<uint32_t>(retry,86400u));
    if(e==Error::Budget)seconds=3600;
    embargo_=next_=now+Ms(seconds)*1000;
}
bool Scheduler::check_now(Ms now) {
    if(busy_ || now<manual_after_ || now<embargo_)return false;
    manual_after_=now+60000;paused_=false;next_=now;return true;
}
void Session::failed_login(Ms now) {
    failures_=std::min(failures_+1,8u);retry_=now+Ms(std::min(300u,1u<<failures_))*1000;
}
void Session::establish(std::string_view cookie,std::string_view csrf,Ms now) {
    clear();
    if(cookie.size()!=64 || csrf.size()!=64)return;
    std::memcpy(cookie_,cookie.data(),64);std::memcpy(csrf_,csrf.data(),64);
    expiry_=now+900000;failures_=0;retry_=0;
}
bool Session::authorized(std::string_view cookie,Ms now) const {
    return expiry_>now && cookie.size()==64 && constant_equal(cookie,cookie_);
}
bool Session::change_allowed(std::string_view cookie,std::string_view csrf,Ms now) const {
    return authorized(cookie,now) && csrf.size()==64 && constant_equal(csrf,csrf_);
}
void Session::clear() {std::memset(cookie_,0,sizeof cookie_);std::memset(csrf_,0,sizeof csrf_);expiry_=0;}
}
