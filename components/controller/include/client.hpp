#pragma once
#include "reliability.hpp"
namespace tsl {
struct FleetDiagnostics {
    const char* endpoint="none";
    const char* detail="none";
    int http_status=0;
    double gps_source_value=-1; // Finite numeric GPS timestamp only; never coordinates.
    char gps_source_text[64]={}; // Original bounded numeric lexeme, no strings/body dump.
    uint32_t attempts_this_boot[3]={}; // Actual transport attempts; reservations are separate.
};
struct HttpResult {
    Error error=Error::None;
    int status=0;
    uint32_t retry_s=0;
    BodyBuffer body;
};
struct FleetIO {
    virtual ~FleetIO()=default;
    virtual Ms now() const=0;
    virtual int64_t utc() const=0;
    virtual bool ready() const=0;
    virtual bool current(uint32_t generation) const=0;
    virtual void request(Endpoint,const Config&,const char* access,const char* form,HttpResult&)=0;
};
// Exactly one worker owns this client in firmware. Fake transport/storage exercise
// this same authentication/request sequencing code in native tests.
class FleetClient {
public:
    FleetClient(Store& s,FleetIO& io,const char* id) : io_(io),id_(id),journal_(s),budget_(s) {}
    Error initialize();
    Error poll(const Config&,uint32_t generation,Observation&);
    Error refresh(const Config&);
    bool refresh_due() const {return access_until_>0 && access_until_<=io_.now();}
    Ms refresh_at() const {return access_until_;}
    uint32_t retry_s() const {return retry_;}
    const BudgetRecord& counts() const {return budget_.counts();}
    FleetDiagnostics diagnostics() const {
        auto d=diagnostic_;
        for(size_t i=0;i<3;++i)d.attempts_this_boot[i]=attempts_[i];
        return d;
    }
private:
    Error account(Endpoint,const Config&);
    Error get(Endpoint,const Config&,Observation&);
    void request(Endpoint,const Config&,const char* access,const char* form,HttpResult&);
    FleetIO& io_;
    const char* id_;
    TokenJournal journal_;
    Budget budget_;
    char access_[4097]={};
    Ms access_until_=0;
    uint32_t retry_=0;
    FleetDiagnostics diagnostic_{};
    uint32_t attempts_[3]={};
};
void wipe(void*,size_t);
}
