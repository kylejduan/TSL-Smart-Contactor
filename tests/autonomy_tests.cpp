#include "test.hpp"
#include "client.hpp"
#include <array>
#include <cstdio>
#include <ctime>
#include <map>

using namespace tsl;
namespace {
// Synthetic time, storage and transport only. Production client/parser,
// journal/budget, scheduler and presence policy run together below.
struct DurableMemory : Store {
    std::map<std::string,std::vector<char>> records;
    ReadResult read(const char* key,void* out,size_t n) override {
        auto it=records.find(key);
        if(it==records.end())return ReadResult::Missing;
        if(it->second.size()!=n)return ReadResult::Failed;
        std::memcpy(out,it->second.data(),n);return ReadResult::Ok;
    }
    bool write(const char* key,const void* in,size_t n) override {
        const auto* p=static_cast<const char*>(in);records[key]={p,p+n};return true;
    }
};
struct SeasonalTransport : FleetIO {
    Ms elapsed=0;
    bool connected=true,service_down=false,asleep=false,away=false;
    std::array<unsigned,3> requests{};
    Ms now() const override {return elapsed;}
    int64_t utc() const override {return 1767225600LL+elapsed/1000;} // 2026-01-01
    bool ready() const override {return connected;}
    bool current(uint32_t generation) const override {return generation==1;}
    uint32_t month() const {
        time_t seconds=utc();tm t{};REQUIRE(gmtime_r(&seconds,&t));
        return (t.tm_year+1900)*12+t.tm_mon+1;
    }
    void request(Endpoint endpoint,const Config&,const char*,const char*,HttpResult& r) override {
        REQUIRE(connected);++requests[static_cast<size_t>(endpoint)];
        if(service_down) {r.status=503;r.error=Error::Server;return;}
        char body[512]={};r.status=200;
        if(endpoint==Endpoint::Refresh) {
            std::snprintf(body,sizeof body,
                "{\"access_token\":\"synthetic-access\",\"refresh_token\":\"synthetic-%u\","
                "\"token_type\":\"Bearer\",\"expires_in\":3600}",requests[2]);
        } else if(endpoint==Endpoint::Status) {
            std::snprintf(body,sizeof body,"{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\","
                "\"state\":\"%s\"}}",asleep?"asleep":"online");
        } else {
            REQUIRE(!asleep); // No location requests to sleeping vehicles.
            std::snprintf(body,sizeof body,"{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\","
                "\"drive_state\":{\"latitude\":0,\"longitude\":%.3f,\"gps_as_of\":%lld}}}",
                away?0.01:0.0,static_cast<long long>(utc()));
        }
        REQUIRE(r.body.append(body,std::strlen(body)));
    }
};
}

TEST(synthetic_year_recovers_from_repeated_outages_without_resetting_policy_or_caps) {
    DurableMemory store;TokenJournal provision(store);
    REQUIRE(provision.provision("synthetic-original")==Error::None);
    SeasonalTransport io;FleetClient client(store,io,"synthetic-client");
    REQUIRE(client.initialize()==Error::None);
    const auto cfg=config();Policy policy;policy.configure(cfg,1,0);Scheduler scheduler;
    uint64_t sequence=0;unsigned failures=0,month_changes=0;
    uint32_t previous_month=0;Ms outage_started=-1;
    // One-minute observations of a full simulated leap-length year. This tests
    // logical recovery, not ESP scheduling, radio, flash endurance or real TLS.
    for(Ms minute=0;minute<366LL*24*60;++minute) {
        io.elapsed=minute*60000;
        const unsigned day=minute/(24*60),hour=(minute/60)%24;
        io.connected=!(day%17==5 && hour>=12 && hour<15);
        io.service_down=day%13==4 && hour>=14 && hour<18;
        io.asleep=hour<6 || hour>=22;
        io.away=hour>=8 && hour<18;
        if(io.ready())scheduler.calendar_month(io.month(),io.now());
        if(previous_month!=io.month()) {++month_changes;previous_month=io.month();}
        if(scheduler.due(io.now())) {
            REQUIRE(scheduler.begin(io.now()));
            Observation observation;observation.generation=1;observation.request=++sequence;
            std::strcpy(observation.vin,cfg.vin);
            auto result=client.poll(cfg,1,observation);
            REQUIRE(result==Error::None || result==Error::Clock || result==Error::Server);
            if(result==Error::None)policy.observe(observation,io.now(),io.utc(),io.ready());
            else ++failures;
            scheduler.finish(io.now(),result,cfg.poll_s,client.retry_s(),0);
        }
        auto decision=policy.tick(io.now());
        if(!io.connected || io.service_down) {
            if(outage_started<0)outage_started=io.now();
            if(io.now()-outage_started>=Ms(cfg.lease_s)*1000) {
                REQUIRE(!decision.auto_home);REQUIRE(!decision.commanded);
            }
        } else outage_started=-1;
        // Allow the full bounded backoff after a returning service before
        // checking automatic recovery. Fresh AWAY and HOME both recur daily.
        if(hour==11) {REQUIRE(!decision.auto_home);REQUIRE(!decision.commanded);}
        if(hour==21) {REQUIRE(decision.auto_home);REQUIRE(decision.commanded);}
        REQUIRE(!client.reauthorization_needed());
        REQUIRE(client.counts().monthly[1]<=kMonthlyDataRequestCap);
        uint64_t daily=0,monthly=0;
        for(size_t i=0;i<3;++i) {daily+=client.counts().daily[i];monthly+=client.counts().monthly[i];}
        REQUIRE(daily<=cfg.daily_cap);REQUIRE(monthly<=cfg.monthly_cap);
    }
    REQUIRE(failures>100);REQUIRE(month_changes==13);
    REQUIRE(io.requests[0]>40000);REQUIRE(io.requests[1]>25000);REQUIRE(io.requests[2]>8000);
    REQUIRE(io.now()>Ms(UINT32_MAX));
    TokenJournal rebooted(store);REQUIRE(rebooted.load()==Error::None);
    REQUIRE(!rebooted.needs_reauth());
}
