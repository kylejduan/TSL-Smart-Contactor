#include "test.hpp"
#include "client.hpp"
#include <map>
#include <deque>
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
struct Reply {Endpoint endpoint;int status;std::string body;};
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
        requests.push_back(e);r.status=reply.status;r.error=http_error(reply.status);r.body.append(reply.body.data(),reply.body.size());
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
TEST(disabled_during_blocked_io_does_not_restore_or_issue_location) {
    Fixture f;Policy policy;policy.configure(config(),1,0);policy.observe(fix(epoch),0,epoch,true);
    REQUIRE(policy.tick(30000).commanded);
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online}};
    f.io.hook=[&](Endpoint e) {if(e==Endpoint::Status) {policy.off(2,31000);f.io.generation=2;REQUIRE(!policy.tick(31000).commanded);}};
    auto o=fix(epoch+32,2);REQUIRE(f.client.poll(config(),1,o)==Error::Unavailable);
    policy.observe(o,32000,epoch+32,true);REQUIRE(!policy.tick(32000).commanded);
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
