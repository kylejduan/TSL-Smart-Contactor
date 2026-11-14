#pragma once
#include "reliability.hpp"
namespace tsl {
// Fixed labels and numeric transport progress only; never request bodies,
// headers, credentials, host addresses or tokens.
struct TransportDiagnostics {
    const char* phase="none";
    uint32_t elapsed_ms=0,connect_ms=0,bytes_written=0,bytes_read=0;
    int connect_state=-1,last_io=0,tls_error=0,mbedtls_error=0,verify_flags=0,system_error=0;
    bool request_may_have_been_sent=false;
};
// Repeated connection failures before any HTTP write may indicate stale local
// DNS/station state. Never reset networking for a server response, ambiguous
// token exchange, certificate failure, or a single transient failure.
class TransportRecovery {
public:
    bool observe(Error,const TransportDiagnostics&,Ms now);
    void clear_streak() {failures_=0;}
    uint32_t attempts() const {return attempts_;}
    uint32_t failures() const {return failures_;}
private:
    uint32_t failures_=0,attempts_=0;
    Ms next_allowed_=0;
};
struct TransportFailure {
    TransportDiagnostics transport{};
    const char* endpoint="none";
    const char* error="none";
    int http_status=0;
    int64_t received_utc_s=0;
};
struct FleetDiagnostics {
    const char* endpoint="none";
    const char* detail="none";
    int http_status=0;
    double gps_source_value=-1; // Finite numeric GPS timestamp only; never coordinates.
    char gps_source_text[64]={}; // Original bounded numeric lexeme, no strings/body dump.
    uint32_t attempts_this_boot[3]={}; // Actual transport attempts; reservations are separate.
    char transaction_id[129]={},response_date[30]={};
    int64_t received_utc_s=0;
    VehicleMetadata vehicle_metadata{};
    double reported_distance_m=-1; // Diagnostic position, even if GPS time is invalid.
    TransportDiagnostics transport{};
    uint32_t network_recoveries=0,last_network_recovery_uptime_s=0;
    int network_recovery_error=0;
    TransportFailure last_failure{};
};
struct HttpResult {
    Error error=Error::None;
    int status=0;
    uint32_t retry_s=0;
    // Only a transport that proves no HTTP request write was attempted may
    // clear this. Unknown transports retain rotation uncertainty by default.
    bool request_may_have_been_sent=true;
    char transaction_id[129]={},response_date[30]={};
    int64_t received_utc_s=0;
    BodyBuffer body;
    TransportDiagnostics transport{};
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
    bool reauthorization_needed() const {return journal_.needs_reauth();}
    uint32_t retry_s() const {return retry_;}
    const BudgetRecord& counts() const {return budget_.counts();}
    void clear_diagnostics() {
        const auto failure=diagnostic_.last_failure;
        diagnostic_={};diagnostic_.last_failure=failure;
    }
    FleetDiagnostics diagnostics() const {
        auto d=diagnostic_;
        for(size_t i=0;i<3;++i)d.attempts_this_boot[i]=attempts_[i];
        return d;
    }
private:
    Error account(Endpoint,const Config&);
    Error get(Endpoint,const Config&,Observation&);
    Error fetch(Endpoint,const Config&,Observation&);
    void request(Endpoint,const Config&,const char* access,const char* form,HttpResult&);
    void diagnose(const char* endpoint,const HttpResult&);
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
