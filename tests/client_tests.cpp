#include "test.hpp"
#include "client.hpp"
#include <map>
#include <deque>
#include <ctime>
using namespace tsl;
namespace {
struct StoreFake : Store {
    std::map<std::string,std::vector<char>> data;bool fail=false;
    ReadResult read(const char* key,void* p,size_t n) override {
        if(!data.count(key))return ReadResult::Missing;
        if(data[key].size()!=n)return ReadResult::Failed;
        std::memcpy(p,data[key].data(),n);return ReadResult::Ok;
    }
    bool write(const char* key,const void* p,size_t n) override {
        if(fail)return false;
        auto* c=static_cast<const char*>(p);data[key]={c,c+n};return true;
    }
};
struct Reply {Endpoint endpoint;int status;std::string body;Error transport_error=Error::None;
    const char* txid="";const char* date="";int64_t received=0;};
const std::string token="{\"access_token\":\"synthetic-access\",\"refresh_token\":\"synthetic-next\",\"token_type\":\"Bearer\",\"expires_in\":1000}";
const std::string asleep="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\"asleep\"}}";
const std::string online="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\"online\"}}";
const std::string location="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000}}}";
struct IO : FleetIO {
    Ms elapsed=0;uint32_t generation=1;bool time_ready=true;
    std::deque<Reply> replies;std::vector<Endpoint> requests;std::function<void(Endpoint)> hook;
    Ms now() const override {return elapsed;}
    int64_t utc() const override {return epoch+elapsed/1000;}
    bool ready() const override {return time_ready;}
    bool current(uint32_t g) const override {return g==generation;}
    void request(Endpoint e,const Config&,const char* access,const char* form,HttpResult& r) override {
        REQUIRE(!replies.empty());auto reply=replies.front();replies.pop_front();REQUIRE(reply.endpoint==e);
        if(e==Endpoint::Refresh) {REQUIRE(form);REQUIRE(!std::strstr(form,"client_secret"));}
        else REQUIRE(std::string(access)=="synthetic-access");
        requests.push_back(e);r.status=reply.status;
        std::snprintf(r.transaction_id,sizeof r.transaction_id,"%s",reply.txid);
        std::snprintf(r.response_date,sizeof r.response_date,"%s",reply.date);r.received_utc_s=reply.received;
        r.error=reply.transport_error==Error::None ? http_error(reply.status) : reply.transport_error;
        r.body.append(reply.body.data(),reply.body.size());
        if(hook)hook(e);
    }
};
struct Fixture {
    StoreFake store;IO io;FleetClient client{store,io,"synthetic-client"};
    Fixture() {TokenJournal j(store);REQUIRE(j.provision("synthetic-original")==Error::None);REQUIRE(client.initialize()==Error::None);}
};
}
TEST(client_checks_status_and_never_fetches_sleeping_location) {
    Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,asleep}};auto o=fix(epoch);
    REQUIRE(f.client.poll(config(),1,o)==Error::None);REQUIRE(o.kind==Evidence::Asleep);REQUIRE(f.io.requests.size()==2);
}
TEST(client_401_exactly_one_refresh_and_one_retry) {
    Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,401,"{}"},
        {Endpoint::Refresh,200,token},{Endpoint::Status,401,"{}"}};auto o=fix(epoch);
    REQUIRE(f.client.poll(config(),1,o)==Error::Authentication);REQUIRE(f.io.requests.size()==4);
    auto d=f.client.diagnostics();REQUIRE(d.attempts_this_boot[0]==2);
    REQUIRE(d.attempts_this_boot[1]==0);REQUIRE(d.attempts_this_boot[2]==2);
}
TEST(client_fetches_location_only_after_online) {
    Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,location}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::None);REQUIRE(o.kind==Evidence::Location);
}
TEST(client_reports_failed_endpoint_status_and_fixed_parser_detail) {
    Fixture f;
    const std::string missing="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":0}}}";
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,missing}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::Malformed);
    REQUIRE(o.vehicle==Vehicle::Online);REQUIRE(o.kind==Evidence::Unknown);
    auto d=f.client.diagnostics();REQUIRE(std::string(d.endpoint)=="location");
    REQUIRE(d.http_status==200);REQUIRE(std::string(d.detail)=="gps_as_of_missing");
    f.io.replies={{Endpoint::Status,403,"private error body"}};
    REQUIRE(f.client.poll(config(),1,o)==Error::Permission);
    d=f.client.diagnostics();REQUIRE(std::string(d.endpoint)=="status");
    REQUIRE(d.http_status==403);REQUIRE(std::string(d.detail)=="missing_permission");
}
TEST(negative_gps_source_never_authorizes_even_with_online_status) {
    Fixture f;
    const std::string invalid="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":0,\"gps_as_of\":-123456789}}}";
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,invalid}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::Malformed);
    REQUIRE(o.vehicle==Vehicle::Online);REQUIRE(o.kind==Evidence::Unknown);
    REQUIRE(f.client.diagnostics().gps_source_value==-123456789);
    REQUIRE(std::string(f.client.diagnostics().detail)=="gps_as_of_out_of_range");
    Policy p;p.configure(config(),1,0);p.observe(o,30000,epoch,true);
    REQUIRE(!p.tick(30000).auto_home);REQUIRE(!p.tick(30000).commanded);
}
TEST(support_metadata_survives_invalid_gps_and_resets_for_next_response) {
    Fixture f;
    const std::string invalid=R"({"response":{"vin":"5YJ3E1EA7KF000001","api_version":83,"drive_state":{"latitude":0,"longitude":0,"gps_as_of":-123456789,"timestamp":1800000000000}}})";
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
        {Endpoint::Location,200,invalid,Error::None,"synthetic-id","Wed, 23 Sep 2026 12:00:00 GMT",epoch}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::Malformed);
    auto d=f.client.diagnostics();REQUIRE(std::string(d.transaction_id)=="synthetic-id");
    REQUIRE(std::string(d.response_date)=="Wed, 23 Sep 2026 12:00:00 GMT");
    REQUIRE(d.received_utc_s==epoch);REQUIRE(d.vehicle_metadata.api_version==83);
    REQUIRE(d.vehicle_metadata.coordinates_valid);REQUIRE(d.reported_distance_m==0);
    REQUIRE(std::string(d.vehicle_metadata.report_timestamp_text)=="1800000000000");
    REQUIRE(o.kind==Evidence::Unknown);
    f.io.replies={{Endpoint::Status,200,asleep}};
    REQUIRE(f.client.poll(config(),1,o)==Error::None);d=f.client.diagnostics();
    REQUIRE(!d.transaction_id[0]);REQUIRE(!d.response_date[0]);REQUIRE(d.received_utc_s==0);
    REQUIRE(d.vehicle_metadata.api_version==-1);REQUIRE(!d.vehicle_metadata.report_timestamp_text[0]);
    REQUIRE(!d.vehicle_metadata.coordinates_valid);REQUIRE(d.reported_distance_m==-1);
}
TEST(reported_position_distance_is_independent_of_invalid_source_time) {
    Fixture f;
    const std::string invalid=R"({"response":{"vin":"5YJ3E1EA7KF000001","drive_state":{"latitude":0.001,"longitude":0,"gps_as_of":-123456789}}})";
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,invalid}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::Malformed);
    auto d=f.client.diagnostics();REQUIRE(d.reported_distance_m>111);REQUIRE(d.reported_distance_m<112);
    REQUIRE(o.kind==Evidence::Unknown);
    Policy p;p.configure(config(),1,0);p.observe(o,30000,epoch,true);
    REQUIRE(!p.tick(30000).auto_home);REQUIRE(!p.tick(30000).commanded);
    const std::string bad=R"({"response":{"vin":"5YJ3E1EA7KF000001","drive_state":{"latitude":null,"longitude":0,"gps_as_of":1800000000}}})";
    f.io.replies={{Endpoint::Status,200,online},{Endpoint::Location,200,bad}};
    REQUIRE(f.client.poll(config(),1,o)==Error::Malformed);
    REQUIRE(f.client.diagnostics().reported_distance_m==-1);
}
TEST(disabled_during_blocked_io_does_not_restore_or_issue_location) {
    Fixture f;Policy policy;policy.configure(config(),1,0);policy.observe(fix(epoch),0,epoch,true);
    REQUIRE(policy.tick(30000).commanded);
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online}};
    f.io.hook=[&](Endpoint e) {if(e==Endpoint::Status) {policy.off(2,31000);f.io.generation=2;REQUIRE(!policy.tick(31000).commanded);}};
    auto o=fix(epoch+32,2);REQUIRE(f.client.poll(config(),1,o)==Error::Unavailable);
    policy.observe(o,32000,epoch+32,true);REQUIRE(!policy.tick(32000).commanded);
}
TEST(network_failure_cannot_keep_an_expired_home_lease_alive) {
    for(auto failure:{Error::Timeout,Error::Transport,Error::TooLarge}) {
        Fixture f;Policy policy;policy.configure(config(),1,0);
        policy.observe(fix(epoch),0,epoch,true);REQUIRE(policy.tick(30000).commanded);
        f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
                      {Endpoint::Location,0,"",failure}};
        f.io.hook=[&](Endpoint endpoint) {
            if(endpoint==Endpoint::Location) {
                // The production policy continues while the worker has not returned.
                REQUIRE(policy.tick(899999).commanded);
                REQUIRE(!policy.tick(900000).commanded);
                REQUIRE(!policy.tick(900000).auto_home);
            }
        };
        auto o=fix(epoch,2);
        REQUIRE(f.client.poll(config(),1,o)==failure);
        REQUIRE(!policy.tick(901000).commanded);
        REQUIRE(f.client.diagnostics().attempts_this_boot[1]==1);
    }
}
TEST(client_permission_rate_billing_server_and_malformed_failures) {
    for(auto pair:{std::pair{403,Error::Permission},{429,Error::RateLimit},{402,Error::Billing},{503,Error::Server},{200,Error::Malformed}}) {
        Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,pair.first,"bad"}};
        auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==pair.second);REQUIRE(f.io.requests.size()==2);
    }
}
TEST(client_revocation_persists_and_prevents_retry) {
    Fixture f;f.io.replies={{Endpoint::Refresh,401,"{\"error\":\"login_required\"}"}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::Reauthorize);
    REQUIRE(f.client.poll(config(),1,o)==Error::Reauthorize);REQUIRE(f.io.requests.size()==1);
}
TEST(client_refresh_before_expiry_and_failed_storage_inhibit) {
    Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,asleep}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::None);REQUIRE(f.client.refresh_at()==900000);
    f.io.elapsed=899999;REQUIRE(!f.client.refresh_due());f.io.elapsed=900000;REQUIRE(f.client.refresh_due());
    f.io.replies={{Endpoint::Refresh,200,token}};f.io.hook=[&](Endpoint) {f.store.fail=true;};
    REQUIRE(f.client.refresh(config())==Error::Storage);
}
TEST(client_caps_prevent_transmission_and_uninitialized_clock_blocks_tls) {
    Fixture f;auto c=config();c.daily_cap=1;f.io.replies={{Endpoint::Refresh,200,token}};auto o=fix(epoch);
    REQUIRE(f.client.poll(c,1,o)==Error::Budget);REQUIRE(f.io.requests.size()==1);
    Fixture g;g.io.time_ready=false;REQUIRE(g.client.poll(config(),1,o)==Error::Clock);REQUIRE(g.io.requests.empty());
    auto d=f.client.diagnostics();REQUIRE(d.attempts_this_boot[0]==0);
    REQUIRE(d.attempts_this_boot[1]==0);REQUIRE(d.attempts_this_boot[2]==1);
    REQUIRE(g.client.diagnostics().attempts_this_boot[2]==0);
}
TEST(monthly_data_cap_skips_status_refresh_and_sleep_renewal) {
    Fixture f;
    Policy policy;policy.configure(config(),1,0);
    policy.observe(fix(epoch),0,epoch,true);
    REQUIRE(policy.tick(30000).commanded);
    time_t utc=f.io.utc();tm t{};REQUIRE(gmtime_r(&utc,&t));
    BudgetRecord record;record.day=utc/86400;
    record.month=static_cast<uint32_t>((t.tm_year+1900)*12+t.tm_mon+1);
    record.monthly[static_cast<size_t>(Endpoint::Location)]=kMonthlyDataRequestCap;
    seal(record);REQUIRE(f.store.write("budget",&record,sizeof record));
    REQUIRE(f.client.initialize()==Error::None);
    f.io.elapsed=600000;auto o=fix(epoch+600,2);
    REQUIRE(f.client.poll(config(),1,o)==Error::Budget);
    REQUIRE(f.io.requests.empty());
    REQUIRE(policy.tick(899999).commanded);
    REQUIRE(!policy.tick(900000).commanded);
}
TEST(request_attempts_count_failures_but_not_refused_storage_or_reboot_credit) {
    Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,503,"{}"}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::Server);
    auto d=f.client.diagnostics();REQUIRE(d.attempts_this_boot[0]==1);REQUIRE(d.attempts_this_boot[2]==1);
    f.store.fail=true;
    REQUIRE(f.client.refresh(config())==Error::Storage);
    REQUIRE(f.client.diagnostics().attempts_this_boot[2]==1);
    f.store.fail=false;
    FleetClient reboot(f.store,f.io,"synthetic-client");REQUIRE(reboot.initialize()==Error::None);
    REQUIRE(reboot.diagnostics().attempts_this_boot[0]==0);
    REQUIRE(reboot.counts().daily[0]>=d.attempts_this_boot[0]);
}
