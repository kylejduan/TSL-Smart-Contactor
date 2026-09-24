#include "protocol.hpp"
#include <cmath>
#include <cstring>
namespace tsl {
const char* error_name(Error e) {
    switch(e) {
    case Error::None:return "none"; case Error::Transport:return "transport";
    case Error::Timeout:return "timeout"; case Error::TooLarge:return "response_too_large";
    case Error::Malformed:return "malformed_data"; case Error::Authentication:return "authentication";
    case Error::Permission:return "missing_permission"; case Error::RateLimit:return "rate_limit";
    case Error::Billing:return "billing"; case Error::Unavailable:return "vehicle_unavailable";
    case Error::Server:return "server"; case Error::Redirect:return "redirect_rejected";
    case Error::Budget:return "local_request_cap"; case Error::Storage:return "storage_fault";
    case Error::Reauthorize:return "reauthorization_required"; case Error::Clock:return "utc_not_ready";
    case Error::Wifi:return "wifi_unavailable";
    }
    return "unknown";
}
Error http_error(int s) {
    if(s>=200 && s<300) return Error::None;
    if(s==401) return Error::Authentication;
    if(s==402) return Error::Billing;
    if(s==403) return Error::Permission;
    if(s==429) return Error::RateLimit;
    if(s==408) return Error::Timeout;
    if(s==404 || s==405 || s==421) return Error::Unavailable;
    if(s>=500) return Error::Server;
    if(s>=300 && s<400) return Error::Redirect;
    return Error::Malformed;
}
Error parse_vehicle(std::string_view body, const char* vin, bool location, Observation& o,const char** detail,double* gps_source_value,char* gps_source_text,size_t source_capacity,VehicleMetadata* metadata) {
    if(detail)*detail="none";
    if(gps_source_value)*gps_source_value=-1;
    if(gps_source_text && source_capacity)gps_source_text[0]=0;
    if(metadata)*metadata={};
    auto reject=[&](const char* reason) {
        if(detail)*detail=reason;
        return Error::Malformed;
    };
    Json j;
    if(!j.parse(body)) return reject("invalid_json");
    int r=j.get(0,"response"); char identity[18] = {};
    if(!j.is(r,Json::Type::Object))return reject("response_not_object");
    if(!j.string(j.get(r,"vin"), identity, sizeof identity) || std::strcmp(identity,vin)) return reject("vin_missing_or_mismatch");
    std::memcpy(o.vin,identity,sizeof identity);
    if(metadata) {
        int64_t version=0;
        if(j.integer(j.get(r,"api_version"),version) && version<=10000)metadata->api_version=version;
    }
    int state=j.get(r,"state");
    if(j.equal(state,"asleep")) o.vehicle=Vehicle::Asleep;
    else if(j.equal(state,"online")) o.vehicle=Vehicle::Online;
    else if(j.equal(state,"offline")) o.vehicle=Vehicle::Offline;
    else o.vehicle=Vehicle::Unknown;
    o.kind=Evidence::Unknown;
    if(!location) {
        if(o.vehicle==Vehicle::Asleep) o.kind=Evidence::Asleep;
        return Error::None;
    }
    int d=j.get(r,"drive_state");
    if(!j.is(d,Json::Type::Object))return reject("drive_state_missing_or_null");
    if(metadata)j.number_text(j.get(d,"timestamp"),metadata->report_timestamp_text,sizeof metadata->report_timestamp_text);
    if(!j.number(j.get(d,"latitude"),o.lat) || !j.number(j.get(d,"longitude"),o.lon) ||
       std::abs(o.lat)>90 || std::abs(o.lon)>180)return reject("coordinates_missing_or_invalid");
    int source=j.get(d,"gps_as_of");
    if(source<0)return reject("gps_as_of_missing");
    if(j.is(source,Json::Type::Null))return reject("gps_as_of_null");
    j.number_text(source,gps_source_text,source_capacity);
    double seconds=0;
    if(!j.number(source,seconds))return reject("gps_as_of_not_numeric");
    if(gps_source_value)*gps_source_value=seconds;
    if(seconds>=1577836800000.0 && seconds<=4102444800000.0)
        return reject("gps_as_of_millisecond_scale");
    if(seconds<1577836800.0 || seconds>4102444800.0)
        return reject("gps_as_of_out_of_range");
    if(std::floor(seconds)!=seconds)return reject("gps_as_of_fractional_seconds");
    // JSON numbers need not use digit-only notation. These bounded whole seconds
    // are exactly representable; never guess units or round a fractional fix up.
    if(!j.integer(source,o.source_s)) {
        o.source_s=static_cast<int64_t>(seconds);
        if(detail)*detail="gps_as_of_numeric_seconds";
    }
    // No documented accuracy field. Do not substitute drive_state.timestamp,
    // native coordinates, or a synthetic location_data response object.
    o.kind=Evidence::Location;
    return Error::None;
}
Error parse_tokens(std::string_view body, Tokens& out) {
    Json j;
    if(!j.parse(body)) return Error::Malformed;
    int error=j.get(0,"error");
    if(j.equal(error,"login_required") || j.equal(error,"invalid_grant")) return Error::Reauthorize;
    int64_t exp=0;
    if(!j.string(j.get(0,"access_token"),out.access,sizeof out.access) ||
       !j.string(j.get(0,"refresh_token"),out.refresh,sizeof out.refresh) ||
       !out.access[0] || !out.refresh[0] || !j.integer(j.get(0,"expires_in"),exp) ||
       exp<60 || exp>86400*7 || !j.equal(j.get(0,"token_type"),"Bearer")) return Error::Malformed;
    // Bearer credentials enter an HTTP header; reject header injection even from
    // an authenticated but malformed token response.
    for(const char* p=out.access;*p;++p) {
        unsigned char c=*p;
        if(!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') ||
             c=='-' || c=='.' || c=='_' || c=='~' || c=='+' || c=='/' || c=='='))return Error::Malformed;
    }
    int scope=j.get(0,"scope");
    if(scope>=0) {
        char scopes[512]={};
        if(!j.string(scope,scopes,sizeof scopes)) return Error::Malformed;
        auto has=[&](std::string_view needed) {
            std::string_view s(scopes);
            while(!s.empty()) { auto end=s.find(' '); if(s.substr(0,end)==needed)return true;
                if(end==s.npos)break;
                s.remove_prefix(end+1); }
            return false;
        };
        if(!has("vehicle_device_data") || !has("vehicle_location")) return Error::Permission;
    }
    out.expires_s=exp; return Error::None;
}
bool parse_config(const Json& j, int o, Config& c) {
    if(!j.string(j.get(o,"vin"),c.vin,sizeof c.vin) ||
       !j.number(j.get(o,"home_lat"),c.home_lat) || !j.number(j.get(o,"home_lon"),c.home_lon)) return false;
    struct Field { const char* name; uint32_t* value; };
    Field fields[]={{"enable_m",&c.enable_m},{"disable_m",&c.disable_m},{"max_age_s",&c.max_age_s},
        {"future_s",&c.future_s},{"lease_s",&c.lease_s},{"sleep_s",&c.sleep_s},{"poll_s",&c.poll_s},
        {"dwell_s",&c.dwell_s},{"daily_cap",&c.daily_cap},{"monthly_cap",&c.monthly_cap}};
    for(auto& f:fields) {
        int k=j.get(o,f.name); int64_t v=0;
        if(k>=0) { if(!j.integer(k,v) || v>UINT32_MAX) return false; *f.value=v; }
    }
    int dry=j.get(o,"dry_run");
    if(dry>=0 && !j.boolean(dry,c.dry_run))return false;
    int region=j.get(o,"region");
    if(region>=0) {
        if(j.equal(region,"NA"))c.region=0;
        else if(j.equal(region,"EU"))c.region=1;
        else return false;
    }
    return valid_config(c);
}
size_t url_encode(std::string_view s, char* out, size_t cap) {
    const char hex[]="0123456789ABCDEF"; size_t n=0;
    for(unsigned char c:s) {
        bool plain=(c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
                   c=='-' || c=='_' || c=='.' || c=='~';
        if(n+(plain?1:3)>=cap)return 0;
        if(plain)out[n++]=c;
        else {out[n++]='%';out[n++]=hex[c>>4];out[n++]=hex[c&15];}
    }
    if(n>=cap)return 0;
    out[n]=0;return n;
}
bool constant_equal(std::string_view a,std::string_view b) {
    if(a.size()!=b.size())return false;
    unsigned diff=0;for(size_t i=0;i<a.size();++i)diff|=unsigned(a[i]^b[i]);
    return diff==0;
}
uint32_t crc32(const void* p,size_t n) {
    auto b=static_cast<const uint8_t*>(p);uint32_t crc=~0u;
    for(size_t i=0;i<n;++i) { crc^=b[i];for(int k=0;k<8;++k)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); }
    return ~crc;
}
bool BodyBuffer::append(const char* p,size_t n) {
    if(overflow_ || n>sizeof(bytes_)-size_) {overflow_=true;return false;}
    std::memcpy(bytes_+size_,p,n);size_+=n;return true;
}
}
