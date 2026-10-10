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
    bool connected=true,service_down=false,asleep=false,away=false,negative_gps=false;
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
                "\"drive_state\":{\"latitude\":0,\"longitude\":%.3f,\"gps_as_of\":%lld,"
                "\"timestamp\":%lld}}}",away?0.01:0.0,
                negative_gps?-123456789LL:static_cast<long long>(utc()),
                static_cast<long long>(utc())*1000);
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

TEST(hold_last_recovers_from_network_and_api_outages_without_canceling_confirmed_home) {
    DurableMemory store;TokenJournal provision(store);
    REQUIRE(provision.provision("synthetic-original")==Error::None);
    SeasonalTransport io;FleetClient client(store,io,"synthetic-client");
    REQUIRE(client.initialize()==Error::None);
    auto cfg=config();cfg.outage_policy=uint8_t(OutagePolicy::HoldLast);
    Policy policy;policy.configure(cfg,1,0);Scheduler scheduler;uint64_t sequence=0;
    for(Ms minute=0;minute<=360;++minute) {
        io.elapsed=minute*60000;
        io.connected=minute<10 || minute>=60;
        io.service_down=minute>=60 && minute<120;
        io.away=minute>=30 && minute<240;
        if(scheduler.due(io.now())) {
            REQUIRE(scheduler.begin(io.now()));
            Observation o;o.generation=1;o.request=++sequence;std::strcpy(o.vin,cfg.vin);
            auto e=client.poll(cfg,1,o);
            REQUIRE(e==Error::None || e==Error::Clock || e==Error::Server);
            if(e==Error::None)policy.observe(o,io.now(),io.utc(),io.ready());
            scheduler.finish(io.now(),e,cfg.poll_s,client.retry_s(),0);
        }
        auto d=policy.tick(io.now());
        if(minute>=15 && minute<120)REQUIRE(d.commanded && d.retained && d.lease_left==0);
        if(minute>=180 && minute<240)REQUIRE(!d.commanded && !d.auto_home);
        if(minute>=300)REQUIRE(d.commanded && d.auto_home);
        REQUIRE(client.counts().monthly[1]<=kMonthlyDataRequestCap);
    }
}

TEST(hold_last_keeps_confirmed_home_at_budget_exhaustion_without_extra_requests) {
    DurableMemory store;TokenJournal provision(store);
    REQUIRE(provision.provision("synthetic-original")==Error::None);
    SeasonalTransport io;FleetClient client(store,io,"synthetic-client");
    REQUIRE(client.initialize()==Error::None);
    auto cfg=config();cfg.outage_policy=uint8_t(OutagePolicy::HoldLast);
    Policy policy;policy.configure(cfg,1,0);
    Observation home;home.generation=1;home.request=1;std::strcpy(home.vin,cfg.vin);
    REQUIRE(client.poll(cfg,1,home)==Error::None);policy.observe(home,0,io.utc(),true);
    REQUIRE(policy.tick(30000).commanded);
    const auto requests=io.requests;io.elapsed=900000;
    auto budget=client.counts();budget.monthly[1]=kMonthlyDataRequestCap;seal(budget);
    REQUIRE(store.write("budget",&budget,sizeof budget));
    FleetClient capped(store,io,"synthetic-client");REQUIRE(capped.initialize()==Error::None);
    Observation none;none.generation=1;none.request=2;std::strcpy(none.vin,cfg.vin);
    REQUIRE(capped.poll(cfg,1,none)==Error::Budget);
    REQUIRE(io.requests==requests);REQUIRE(policy.tick(io.now()).retained);
    REQUIRE(policy.tick(io.now()).commanded && policy.tick(io.now()).lease_left==0);
}

TEST(selected_report_profile_sleeps_through_night_but_stops_at_fixed_ceiling) {
    DurableMemory store;TokenJournal provision(store);
    REQUIRE(provision.provision("synthetic-original")==Error::None);
    SeasonalTransport io;io.negative_gps=true;
    FleetClient client(store,io,"synthetic-client");REQUIRE(client.initialize()==Error::None);
    auto cfg=config();cfg.position_basis=uint8_t(PositionBasis::VehicleReport);
    cfg.max_age_s=600;cfg.lease_s=600;cfg.poll_s=540;REQUIRE(valid_config(cfg));
    Policy policy;policy.configure(cfg,1,0);Scheduler scheduler;
    uint64_t sequence=0;int64_t last_report=0;unsigned locations_before_sleep=0;
    // The car supplies one HOME report, then sleeps for 36 hours. No location
    // response or advancing report timestamp is fabricated during sleep.
    for(Ms second=0;second<=37*60*60;second+=30) {
        io.elapsed=second*1000;io.asleep=second>0 && second<36*60*60;
        scheduler.calendar_month(io.month(),io.now());
        if(scheduler.due(io.now())) {
            REQUIRE(scheduler.begin(io.now()));
            Observation o;o.generation=1;o.request=++sequence;std::strcpy(o.vin,cfg.vin);
            auto result=client.poll(cfg,1,o);REQUIRE(result==Error::None);
            REQUIRE(o.position_basis==PositionBasis::VehicleReport);
            if(o.kind==Evidence::Location) {
                REQUIRE(!io.asleep);last_report=o.source_s;
                REQUIRE(client.diagnostics().gps_source_value<0);
            } else REQUIRE(o.kind==Evidence::Asleep);
            policy.observe(o,io.now(),io.utc(),true);
            scheduler.finish(io.now(),result,cfg.poll_s,client.retry_s(),0);
        }
        const auto decision=policy.tick(io.now());
        if(second==0) {locations_before_sleep=io.requests[1];REQUIRE(locations_before_sleep==1);}
        if(io.asleep) {
            REQUIRE(io.requests[1]==locations_before_sleep);
            REQUIRE(decision.last_source_s==last_report);
        }
        if(second>=30 && second<24*60*60)REQUIRE(decision.commanded);
        if(second>=24*60*60 && second<36*60*60) {
            REQUIRE(!decision.commanded);REQUIRE(!decision.auto_home);
        }
        if(second>=36*60*60+cfg.poll_s)REQUIRE(decision.commanded);
        REQUIRE(!client.reauthorization_needed());
    }
    REQUIRE(io.requests[2]>1); // Runtime refreshes continue during sleep.
    REQUIRE(last_report>1767225600LL+36*60*60);
}

TEST(selected_report_profile_31_day_budget_and_next_month_recovery) {
    for(uint32_t prior_attempts:{0u,64u}) {
        DurableMemory store;TokenJournal provision(store);
        REQUIRE(provision.provision("synthetic-original")==Error::None);
        SeasonalTransport io;io.negative_gps=true;
        if(prior_attempts) {
            // Other controller attempts earlier in this UTC month, all counted
            // conservatively. No external account usage is inferred here.
            BudgetRecord prior;prior.day=io.utc()/86400;prior.month=io.month();
            prior.daily[1]=prior_attempts;prior.monthly[1]=prior_attempts;seal(prior);
            REQUIRE(store.write("budget",&prior,sizeof prior));
        }
        FleetClient client(store,io,"synthetic-client");REQUIRE(client.initialize()==Error::None);
        auto cfg=config();cfg.position_basis=uint8_t(PositionBasis::VehicleReport);
        cfg.max_age_s=600;cfg.lease_s=600;cfg.poll_s=540;REQUIRE(valid_config(cfg));
        Policy policy;policy.configure(cfg,1,0);Scheduler scheduler;
        uint64_t sequence=0;Ms first_monthly_failure=-1;
        unsigned january_locations=0,budget_failures=0;uint32_t january_reserved=0;
        constexpr Ms month_end=31LL*24*60*60*1000;
        // Instantaneous successful responses are synthetic best-case throughput.
        // Real deadlines/backoff can reduce calls or interrupt authorization.
        for(Ms now=0;now<month_end+2*60*60*1000;now+=30000) {
            io.elapsed=now;scheduler.calendar_month(io.month(),now);
            if(scheduler.due(now)) {
                REQUIRE(scheduler.begin(now));
                Observation o;o.generation=1;o.request=++sequence;std::strcpy(o.vin,cfg.vin);
                auto result=client.poll(cfg,1,o);
                REQUIRE(result==Error::None || result==Error::Budget);
                if(result==Error::None)policy.observe(o,now,io.utc(),true);
                else {
                    ++budget_failures;
                    if(first_monthly_failure<0 && client.counts().monthly[1]==kMonthlyDataRequestCap)
                        first_monthly_failure=now;
                }
                scheduler.finish(now,result,cfg.poll_s,client.retry_s(),0);
            }
            const auto decision=policy.tick(now);
            REQUIRE(client.counts().monthly[1]<=kMonthlyDataRequestCap);
            if(now<month_end) {
                january_locations=io.requests[1];january_reserved=client.counts().monthly[1];
                if(!prior_attempts && now>=30000)REQUIRE(decision.commanded);
                // Daily exhaustion may recover tomorrow. Only monthly exhaustion
                // must inhibit all later renewals until the new UTC month.
                if(first_monthly_failure>=0 && now>=first_monthly_failure+cfg.lease_s*1000)
                    REQUIRE(!decision.commanded);
            }
            if(now>=month_end+3600000)REQUIRE(decision.commanded);
            REQUIRE(!client.reauthorization_needed());
        }
        if(!prior_attempts) {
            REQUIRE(budget_failures==0);REQUIRE(january_locations==4960);
            REQUIRE(january_reserved==4960); // $9.92 at the pinned $0.002 rate.
        } else {
            REQUIRE(budget_failures>0);REQUIRE(first_monthly_failure>=0);
            REQUIRE(first_monthly_failure<month_end);
            REQUIRE(january_reserved==kMonthlyDataRequestCap);
            REQUIRE(january_locations+prior_attempts<=kMonthlyDataRequestCap);
        }
        REQUIRE(io.requests[1]>january_locations); // New UTC month resumes polling.
    }
}
