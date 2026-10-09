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
    const char* txid="";const char* date="";int64_t received=0;bool request_may_have_been_sent=true;
    uint32_t retry_s=0;};
const std::string token="{\"access_token\":\"synthetic-access\",\"refresh_token\":\"synthetic-next\",\"token_type\":\"Bearer\",\"expires_in\":1000}";
const std::string asleep="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\"asleep\"}}";
const std::string online="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\"online\"}}";
const std::string location="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000}}}";
std::string report_location(const std::string& timestamp,double longitude=0) {
    return "{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":"+
        std::to_string(longitude)+",\"gps_as_of\":-123456789"+
        (timestamp.empty() ? "" : ",\"timestamp\":"+timestamp)+"}}}";
}
struct IO : FleetIO {
    Ms elapsed=0;uint32_t generation=1;bool time_ready=true;
    int64_t utc_base=epoch;
    std::deque<Reply> replies;std::vector<Endpoint> requests;std::function<void(Endpoint)> hook;
    Ms now() const override {return elapsed;}
    int64_t utc() const override {return utc_base+elapsed/1000;}
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
        r.retry_s=reply.retry_s;
        r.request_may_have_been_sent=reply.request_may_have_been_sent;
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
TEST(report_mode_client_parser_policy_authorize_then_reject_duplicate_and_departure) {
    Fixture f;auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    Policy p;p.configure(c,1,0);
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
                  {Endpoint::Location,200,report_location("1800000000123")}};
    auto o=fix(epoch);REQUIRE(f.client.poll(c,1,o)==Error::None);
    REQUIRE(o.position_basis==PositionBasis::VehicleReport);REQUIRE(o.source_s==epoch);
    REQUIRE(f.client.diagnostics().gps_source_value==-123456789);
    p.observe(o,0,epoch,true);REQUIRE(!p.tick(29999).commanded);REQUIRE(p.tick(30000).commanded);
    f.io.elapsed=600000;f.io.replies={{Endpoint::Status,200,online},
        {Endpoint::Location,200,report_location("1800000600123")}};
    o=fix(epoch+600,2);REQUIRE(f.client.poll(c,1,o)==Error::None);p.observe(o,600000,epoch+600,true);
    REQUIRE(p.tick(600000).commanded);REQUIRE(p.tick(600000).lease_left==900000);
    f.io.elapsed=601000;f.io.replies={{Endpoint::Status,200,online},
        {Endpoint::Location,200,report_location("1800000600123")}};
    o=fix(epoch+601,3);REQUIRE(f.client.poll(c,1,o)==Error::None);p.observe(o,601000,epoch+601,true);
    REQUIRE(p.tick(601000).lease_left==899000);
    f.io.elapsed=602000;f.io.replies={{Endpoint::Status,200,online},
        {Endpoint::Location,200,report_location("1800000602000",0.002)}};
    o=fix(epoch+602,4);REQUIRE(f.client.poll(c,1,o)==Error::None);p.observe(o,602000,epoch+602,true);
    REQUIRE(!p.tick(602000).auto_home);REQUIRE(!p.tick(602000).commanded);
}
TEST(synthetic_invalid_gps_and_valid_report_obey_selected_basis_and_age) {
    // Fully synthetic numbers exercise the distinction without retaining a live capture.
    constexpr int64_t completion_utc=1800000000;
    const std::string synthetic_numbers=R"({"response":{"vin":"5YJ3E1EA7KF000001","drive_state":{"latitude":0,"longitude":0,"gps_as_of":-123456789,"timestamp":1800000000643}}})";
    Fixture strict;strict.io.utc_base=completion_utc;
    strict.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
                       {Endpoint::Location,200,synthetic_numbers,Error::None,"","",completion_utc}};
    auto strict_observation=fix(completion_utc);
    REQUIRE(strict.client.poll(config(),1,strict_observation)==Error::SourceTime);
    Policy strict_policy;strict_policy.configure(config(),1,0);
    strict_policy.observe(strict_observation,0,completion_utc,true);
    REQUIRE(!strict_policy.tick(30000).auto_home);
    REQUIRE(!strict_policy.tick(30000).commanded);
    REQUIRE(strict.client.diagnostics().gps_source_value==-123456789);

    Fixture report;report.io.utc_base=completion_utc;
    auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    report.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
                       {Endpoint::Location,200,synthetic_numbers,Error::None,"","",completion_utc}};
    auto observation=fix(completion_utc);
    REQUIRE(report.client.poll(c,1,observation)==Error::None);
    REQUIRE(observation.source_s==completion_utc);
    REQUIRE(observation.position_basis==PositionBasis::VehicleReport);
    Policy report_policy;report_policy.configure(c,1,0);
    report_policy.observe(observation,0,completion_utc,true);
    REQUIRE(report_policy.tick(0).lease_left==900000);
    REQUIRE(report_policy.tick(30000).auto_home);REQUIRE(report_policy.tick(30000).commanded);

    report.io.elapsed=121000;
    report.io.replies={{Endpoint::Status,200,online},
        {Endpoint::Location,200,synthetic_numbers,Error::None,"","",completion_utc+121}};
    observation=fix(completion_utc+121,2);
    REQUIRE(report.client.poll(c,1,observation)==Error::None);
    REQUIRE(observation.source_s==completion_utc); // Later receipt cannot refresh it.
    Policy fresh_policy;fresh_policy.configure(c,1,121000);
    fresh_policy.observe(observation,121000,report.io.utc(),true);
    REQUIRE(!fresh_policy.tick(151000).auto_home);REQUIRE(!fresh_policy.tick(151000).commanded);
    report_policy.observe(observation,121000,report.io.utc(),true);
    REQUIRE(report_policy.tick(121000).lease_left==779000);
    REQUIRE(!report_policy.tick(900000).auto_home);
}
TEST(report_mode_errors_and_nonfresh_reports_do_not_renew_existing_permission) {
    for(const char* timestamp:{"", "null", "1800000600", "1800000600000.5", "1800000479000",
                              "1800000631000", "1800000000000", "1799999999000"}) {
        Fixture f;auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
        Policy p;p.configure(c,1,0);
        f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
                      {Endpoint::Location,200,report_location("1800000000000")}};
        auto o=fix(epoch);REQUIRE(f.client.poll(c,1,o)==Error::None);p.observe(o,0,epoch,true);
        REQUIRE(p.tick(30000).commanded);
        f.io.elapsed=600000;f.io.replies={{Endpoint::Status,200,online},
                                       {Endpoint::Location,200,report_location(timestamp)}};
        o=fix(epoch+600,2);auto result=f.client.poll(c,1,o);
        REQUIRE(result==Error::None || result==Error::ReportTime);
        if(result==Error::None)p.observe(o,600000,epoch+600,true);
        REQUIRE(p.tick(899999).commanded);REQUIRE(!p.tick(900000).auto_home);
    }
}
TEST(report_mode_never_polls_location_without_online_or_accepts_conflicting_state) {
    auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    for(const char* state:{"asleep","offline","unknown"}) {
        const std::string status="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\""+std::string(state)+"\"}}";
        Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,status}};
        auto o=fix(epoch);REQUIRE(f.client.poll(c,1,o)==Error::None);REQUIRE(f.io.requests.size()==2);
        Policy p;p.configure(c,1,0);p.observe(o,30000,epoch,true);REQUIRE(!p.tick(30000).auto_home);
        Fixture g;auto wire=report_location("1800000000000");
        wire.insert(wire.find("\"drive_state\""),"\"state\":\""+std::string(state)+"\",");
        g.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,wire}};
        o=fix(epoch);REQUIRE(g.client.poll(c,1,o)==Error::Unavailable);REQUIRE(o.kind==Evidence::Unknown);
    }
}
TEST(report_mode_sleep_renews_only_prior_report_and_preserves_its_timestamp) {
    Fixture f;auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    Policy p;p.configure(c,1,0);
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},
                  {Endpoint::Location,200,report_location("1800000000000")}};
    auto o=fix(epoch);REQUIRE(f.client.poll(c,1,o)==Error::None);p.observe(o,0,epoch,true);
    f.io.elapsed=600000;f.io.replies={{Endpoint::Status,200,asleep}};
    o=fix(epoch+600,2);REQUIRE(f.client.poll(c,1,o)==Error::None);
    REQUIRE(o.position_basis==PositionBasis::VehicleReport);REQUIRE(o.kind==Evidence::Asleep);
    p.observe(o,600000,epoch+600,true);
    REQUIRE(p.tick(600000).last_source_s==epoch);REQUIRE(p.tick(1499999).auto_home);
    REQUIRE(!p.tick(1500000).auto_home);
}
TEST(client_reports_failed_endpoint_status_and_fixed_parser_detail) {
    Fixture f;
    const std::string missing="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"drive_state\":{\"latitude\":0,\"longitude\":0}}}";
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,missing}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::SourceTime);
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
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::SourceTime);
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
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::SourceTime);
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
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::SourceTime);
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
TEST(home_change_during_location_response_discards_late_data_and_diagnostics) {
    Fixture f;
    f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,online},{Endpoint::Location,200,location}};
    auto first=fix(epoch);REQUIRE(f.client.poll(config(),1,first)==Error::None);
    REQUIRE(std::string(f.client.diagnostics().endpoint)=="location");
    const std::string stale_location=R"({"response":{"vin":"5YJ3E1EA7KF000001","drive_state":{"latitude":1,"longitude":0,"gps_as_of":1800000060}}})";
    f.io.replies={{Endpoint::Status,200,online},{Endpoint::Location,200,stale_location}};
    f.io.hook=[&](Endpoint e) {if(e==Endpoint::Location)f.io.generation=2;};
    auto late=fix(epoch+60);REQUIRE(f.client.poll(config(),1,late)==Error::Unavailable);
    REQUIRE(late.kind==Evidence::Unknown);REQUIRE(late.lat==0);
    auto d=f.client.diagnostics();
    REQUIRE(std::string(d.endpoint)=="status");
    REQUIRE(d.reported_distance_m==-1);
    REQUIRE(d.attempts_this_boot[1]==2);
    f.client.clear_diagnostics();d=f.client.diagnostics();
    REQUIRE(std::string(d.endpoint)=="none");
    REQUIRE(d.attempts_this_boot[1]==2);
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
    REQUIRE(f.client.reauthorization_needed());
    REQUIRE(f.client.poll(config(),1,o)==Error::Reauthorize);REQUIRE(f.io.requests.size()==1);
}
TEST(missing_refresh_token_is_visible_without_a_fleet_request) {
    StoreFake store;IO io;FleetClient client(store,io,"synthetic-client");
    REQUIRE(client.initialize()==Error::Reauthorize);
    REQUIRE(client.reauthorization_needed());
    auto o=fix(epoch);REQUIRE(client.poll(config(),1,o)==Error::Reauthorize);
    REQUIRE(io.requests.empty());
}
TEST(client_refresh_before_expiry_and_failed_storage_inhibit) {
    Fixture f;f.io.replies={{Endpoint::Refresh,200,token},{Endpoint::Status,200,asleep}};
    auto o=fix(epoch);REQUIRE(f.client.poll(config(),1,o)==Error::None);REQUIRE(f.client.refresh_at()==900000);
    f.io.elapsed=899999;REQUIRE(!f.client.refresh_due());f.io.elapsed=900000;REQUIRE(f.client.refresh_due());
    f.io.replies={{Endpoint::Refresh,200,token}};f.io.hook=[&](Endpoint) {f.store.fail=true;};
    REQUIRE(f.client.refresh(config())==Error::Storage);
}
TEST(refresh_connection_failures_do_not_exhaust_rotation_recovery) {
    Fixture f;
    for(int i=0;i<6;++i) {
        Reply unsent{Endpoint::Refresh,0,"",Error::Transport};
        unsent.request_may_have_been_sent=false;f.io.replies={unsent};
        REQUIRE(f.client.refresh(config())==Error::Transport);
        REQUIRE(!f.client.reauthorization_needed());f.io.elapsed+=3600000;
    }
    FleetClient reboot(f.store,f.io,"synthetic-client");
    REQUIRE(reboot.initialize()==Error::None);
    f.io.replies={{Endpoint::Refresh,200,token}};
    REQUIRE(reboot.refresh(config())==Error::None);
    REQUIRE(f.io.requests.size()==7);
}
TEST(refresh_maybe_sent_failures_keep_the_bounded_recovery_limit) {
    for(auto response:{Reply{Endpoint::Refresh,0,"",Error::Timeout},
                       Reply{Endpoint::Refresh,429,"{}"},Reply{Endpoint::Refresh,503,"{}"}}) {
        Fixture f;
        for(uint32_t i=0;i<kRefreshRecoveryAttempts;++i) {
            f.io.replies={response};
            REQUIRE(f.client.refresh(config())==
                    (response.status ? http_error(response.status) : Error::Timeout));
            f.io.elapsed+=60000;
        }
        REQUIRE(f.client.refresh(config())==Error::Reauthorize);
        REQUIRE(f.client.reauthorization_needed());REQUIRE(f.io.requests.size()==kRefreshRecoveryAttempts);
    }
}
TEST(refresh_transient_outage_recovers_after_more_than_three_ambiguous_failures) {
    Fixture f;Scheduler scheduler;
    for(unsigned i=0;i<6;++i) {
        f.io.elapsed=scheduler.next();REQUIRE(scheduler.begin(f.io.elapsed));
        f.io.replies={{Endpoint::Refresh,503,"{}"}};
        auto error=f.client.refresh(config());REQUIRE(error==Error::Server);
        REQUIRE(!f.client.reauthorization_needed());
        scheduler.finish(f.io.elapsed,error,600,f.client.retry_s(),0);
    }
    REQUIRE(scheduler.next()>3600000);REQUIRE(!scheduler.paused());
    FleetClient reboot(f.store,f.io,"synthetic-client");REQUIRE(reboot.initialize()==Error::None);
    f.io.elapsed=scheduler.next();f.io.replies={{Endpoint::Refresh,200,token}};
    REQUIRE(reboot.refresh(config())==Error::None);REQUIRE(!reboot.reauthorization_needed());
    REQUIRE(reboot.refresh_at()==f.io.elapsed+900000);
    TokenJournal stored(f.store);REQUIRE(stored.load()==Error::None);
    REQUIRE(std::string(stored.current())=="synthetic-next");
}
TEST(refresh_recovery_never_moves_first_uncertain_twenty_four_hour_ceiling) {
    Fixture f;f.io.replies={{Endpoint::Refresh,0,"",Error::Timeout}};
    REQUIRE(f.client.refresh(config())==Error::Timeout);
    f.io.elapsed=Ms(23)*3600000;f.io.replies={{Endpoint::Refresh,503,"{}"}};
    REQUIRE(f.client.refresh(config())==Error::Server);
    f.io.elapsed=Ms(24)*3600000;
    REQUIRE(f.client.refresh(config())==Error::Reauthorize);
    REQUIRE(f.io.requests.size()==2);REQUIRE(f.client.reauthorization_needed());
}
TEST(standalone_refresh_discards_retry_after_from_previous_responses) {
    Fixture f;Reply limited{Endpoint::Refresh,429,"{}"};limited.retry_s=86400;
    f.io.replies={limited};REQUIRE(f.client.refresh(config())==Error::RateLimit);
    REQUIRE(f.client.retry_s()==86400);
    f.io.elapsed=60000;f.io.replies={{Endpoint::Refresh,503,"{}"}};
    REQUIRE(f.client.refresh(config())==Error::Server);REQUIRE(f.client.retry_s()==0);
    f.io.replies={{Endpoint::Refresh,200,token}};
    REQUIRE(f.client.refresh(config())==Error::None);REQUIRE(f.client.retry_s()==0);
}
TEST(refresh_unsent_restore_failure_is_a_critical_storage_error) {
    Fixture f;Reply unsent{Endpoint::Refresh,0,"",Error::Transport};
    unsent.request_may_have_been_sent=false;f.io.replies={unsent};
    f.io.hook=[&](Endpoint) {f.store.fail=true;};
    REQUIRE(f.client.refresh(config())==Error::Storage);
    REQUIRE(f.client.refresh_at()==0);REQUIRE(f.io.requests.size()==1);
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
