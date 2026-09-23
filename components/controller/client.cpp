#include "client.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
namespace tsl {
void wipe(void* p,size_t n) {
    auto* bytes=static_cast<volatile unsigned char*>(p);
    while(n--)*bytes++=0;
}
Error FleetClient::initialize() {
    auto e=journal_.load();
    return budget_.load()!=Error::None ? Error::Storage : e;
}
void FleetClient::request(Endpoint endpoint,const Config& cfg,const char* access,const char* form,HttpResult& result) {
    auto& count=attempts_[static_cast<size_t>(endpoint)];
    if(count<UINT32_MAX)++count;
    io_.request(endpoint,cfg,access,form,result);
}
Error FleetClient::account(Endpoint endpoint,const Config& cfg) {
    if(!io_.ready())return Error::Clock;
    time_t utc=io_.utc();tm t{};
    if(!gmtime_r(&utc,&t))return Error::Clock;
    return budget_.take(endpoint,utc/86400,(t.tm_year+1900)*12+t.tm_mon+1,cfg.daily_cap,cfg.monthly_cap);
}
Error FleetClient::refresh(const Config& cfg) {
    auto e=account(Endpoint::Refresh,cfg);
    if(e!=Error::None)return e;
    e=journal_.begin(io_.utc());
    if(e!=Error::None)return e;
    char token[6145]={},id[385]={},form[6656]={};
    if(!url_encode(journal_.current(),token,sizeof token) || !url_encode(id_,id,sizeof id)) {
        journal_.release();return Error::Malformed;
    }
    std::snprintf(form,sizeof form,"grant_type=refresh_token&client_id=%s&refresh_token=%s",id,token);
    HttpResult result;request(Endpoint::Refresh,cfg,nullptr,form,result);
    diagnostic_={"refresh",error_name(result.error),result.status};
    retry_=std::max(retry_,result.retry_s);wipe(form,sizeof form);wipe(token,sizeof token);
    e=result.error;Tokens tokens;
    if(e==Error::None) {
        e=parse_tokens(result.body.view(),tokens);
        if(e!=Error::None)diagnostic_.detail="token_response_rejected";
    }
    else if(result.status==400 || result.status==401) {
        Json j;
        if(j.parse(result.body.view()) && (j.equal(j.get(0,"error"),"login_required") ||
           j.equal(j.get(0,"error"),"invalid_grant")))e=Error::Reauthorize;
        else if(result.status==401)e=Error::Reauthorize;
    }
    if(e==Error::None) {
        e=journal_.finish(tokens.refresh);
        if(e==Error::None) {
            std::strcpy(access_,tokens.access);
            auto margin=std::min<uint32_t>(300u,tokens.expires_s/10);
            access_until_=io_.now()+Ms(tokens.expires_s-margin)*1000;
        }
    } else if(e==Error::Reauthorize || e==Error::Permission)e=journal_.revoke();
    journal_.release();wipe(&tokens,sizeof tokens);wipe(&result,sizeof result);
    return e;
}
Error FleetClient::get(Endpoint endpoint,const Config& cfg,Observation& o) {
    for(int attempt=0;attempt<2;++attempt) {
        // OFF or a home/VIN change also prevents a queued 401 retry from starting.
        if(!io_.current(o.generation))return Error::Unavailable;
        auto e=account(endpoint,cfg);if(e!=Error::None)return e;
        HttpResult result;request(endpoint,cfg,access_,nullptr,result);
        diagnostic_={endpoint==Endpoint::Location?"location":"status",error_name(result.error),result.status};
        retry_=std::max(retry_,result.retry_s);
        if(result.error==Error::Authentication && attempt==0) {
            if(!io_.current(o.generation))return Error::Unavailable;
            e=refresh(cfg);if(e!=Error::None)return e;
            continue;
        }
        if(result.error!=Error::None)return result.error;
        return parse_vehicle(result.body.view(),cfg.vin,endpoint==Endpoint::Location,o,
                             &diagnostic_.detail,&diagnostic_.gps_source_value,
                             diagnostic_.gps_source_text,sizeof diagnostic_.gps_source_text);
    }
    return Error::Authentication;
}
Error FleetClient::poll(const Config& cfg,uint32_t generation,Observation& o) {
    retry_=0;
    if(journal_.needs_reauth())return Error::Reauthorize;
    if(!io_.ready())return Error::Clock;
    if(!io_.current(generation))return Error::Unavailable;
    if(access_until_<=io_.now()) {auto e=refresh(cfg);if(e!=Error::None)return e;}
    auto e=get(Endpoint::Status,cfg,o);
    if(e!=Error::None || o.vehicle!=Vehicle::Online)return e;
    if(!io_.current(generation))return Error::Unavailable;
    const auto status_vehicle=o.vehicle;
    e=get(Endpoint::Location,cfg,o);
    // Keep the last explicit connectivity check distinct from GPS acceptance.
    // The location response may omit state or contain unusable source time.
    o.vehicle=status_vehicle;
    return e;
}
}
