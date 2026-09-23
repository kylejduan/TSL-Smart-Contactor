#include "test.hpp"
#include <cmath>
#include <limits>
using namespace tsl;
TEST(boot_uncommissioned_dryrun_and_reset) {
    Policy p;REQUIRE(!p.tick(60000).commanded);
    auto c=config();c.commissioned=false;p.configure(c,1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(!p.tick(30000).commanded);REQUIRE(!p.timed_on(60,30000));
    Policy dry;c.commissioned=true;c.dry_run=true;dry.configure(c,1,0);dry.observe(fix(epoch),0,epoch,true);
    REQUIRE(dry.tick(30000).desired);REQUIRE(!dry.tick(30000).commanded);
    Policy reboot;reboot.configure(config(),1,0);REQUIRE(!reboot.tick(50000).auto_home);
    REQUIRE(!reboot.tick(50000).timed);REQUIRE(!reboot.tick(50000).commanded);
}
TEST(home_dwell_away_and_hysteresis) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(!p.tick(29999).commanded);REQUIRE(p.tick(30000).commanded);
    p.observe(fix(epoch+31,2,0.00135),31000,epoch+31,true);REQUIRE(p.tick(31000).commanded);
    p.observe(fix(epoch+32,3,0.002),32000,epoch+32,true);REQUIRE(!p.tick(32000).commanded);
    p.observe(fix(epoch+33,4,0.00135),33000,epoch+33,true);REQUIRE(!p.tick(33000).auto_home);
    p.observe(fix(epoch+34,5),34000,epoch+34,true);REQUIRE(!p.tick(61999).commanded);
    REQUIRE(p.tick(62000).commanded);
}
TEST(lease_t0_expires_at_900) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(899999).commanded);REQUIRE(!p.tick(900000).commanded);
}
TEST(fresh_fix_at_600_renews_without_pulse) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);REQUIRE(p.tick(30000).commanded);
    p.observe(fix(epoch+600,2),600000,epoch+600,true);
    REQUIRE(p.tick(600000).commanded);REQUIRE(p.tick(1499999).commanded);
    REQUIRE(!p.tick(1500000).commanded);
}
TEST(stale_duplicate_future_and_out_of_order_never_renew) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    p.observe(fix(epoch,2),600000,epoch+600,true);
    p.observe(fix(epoch-1,3),600000,epoch+600,true);
    p.observe(fix(epoch+631,4),600000,epoch+600,true);
    p.observe(fix((epoch+600)*1000,5),600000,epoch+600,true);
    REQUIRE(!p.tick(900000).commanded);
}
TEST(source_age_is_subtracted_and_future_has_no_bonus) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch-120),0,epoch,true);
    REQUIRE(p.tick(0).lease_left==780000);REQUIRE(!p.tick(780000).auto_home);
    Policy q;q.configure(config(),1,0);q.observe(fix(epoch+30),0,epoch,true);
    REQUIRE(q.tick(0).lease_left==900000);
}
TEST(sleep_renews_only_continuous_home_and_source_ceiling) {
    Policy p;p.configure(config(),1,0);auto sleep=fix(epoch);sleep.kind=Evidence::Asleep;
    p.observe(sleep,0,epoch,true);REQUIRE(!p.tick(0).auto_home);
    p.observe(fix(epoch,2),0,epoch,true);
    for(int s=600;s<86400;s+=600) {
        sleep.request=3+s/600;p.observe(sleep,Ms(s)*1000,epoch+s,true);
        REQUIRE(p.tick(Ms(s)*1000).auto_home);
        REQUIRE(p.tick(Ms(s)*1000).lease_left<=86400000-Ms(s)*1000);
        REQUIRE(p.tick(Ms(s)*1000).last_source_s==epoch);
    }
    REQUIRE(!p.tick(86400000).auto_home);
    sleep.request=1000;p.observe(sleep,86400001,epoch+86400,true);REQUIRE(!p.tick(86400001).auto_home);
    Policy expired;expired.configure(config(),1,0);expired.observe(fix(epoch),0,epoch,true);
    sleep.request=2;expired.observe(sleep,900000,epoch+900,true);REQUIRE(!expired.tick(900000).auto_home);
}
TEST(duplicate_sleep_response_does_not_extend) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    auto o=fix(epoch,2);o.kind=Evidence::Asleep;p.observe(o,600000,epoch+600,true);
    p.observe(o,1200000,epoch+1200,true);REQUIRE(!p.tick(1500000).auto_home);
}
TEST(away_and_revocation_invalidate_auto_during_override) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.timed_on(3600,30000));p.observe(fix(epoch+40,2,1),40000,epoch+40,true);
    REQUIRE(!p.tick(40000).auto_home);REQUIRE(p.tick(40000).commanded);
    p.observe(fix(epoch+50,3),50000,epoch+50,true);REQUIRE(p.tick(50000).auto_home);
    auto o=fix(epoch+51,4);o.kind=Evidence::Revoked;p.observe(o,51000,epoch+51,true);
    REQUIRE(!p.tick(51000).auto_home);REQUIRE(p.tick(51000).commanded);
}
TEST(timed_does_not_create_home_and_disabled_wins) {
    Policy p;p.configure(config(),1,0);REQUIRE(p.timed_on(3600,0));
    REQUIRE(p.tick(30000).commanded);REQUIRE(!p.tick(30000).auto_home);
    p.off(2,31000);p.observe(fix(epoch+32,99,0,1),32000,epoch+32,true);
    REQUIRE(!p.tick(32000).commanded);REQUIRE(!p.timed_on(3600,32000));
    REQUIRE(!p.tick(32000).auto_home);
}
TEST(timed_expiry_uses_independent_auto) {
    Policy p;p.configure(config(),1,0);REQUIRE(p.timed_on(60,0));p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(30000).commanded);REQUIRE(p.tick(60000).commanded);REQUIRE(!p.tick(60000).timed);
    Policy q;q.configure(config(),1,0);REQUIRE(q.timed_on(60,0));REQUIRE(q.tick(30000).commanded);
    REQUIRE(!q.tick(60000).commanded);REQUIRE(!q.timed_on(28801,70000));
}
TEST(offline_online_errors_and_bad_gps_do_not_extend) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    for(uint64_t i=2;i<8;++i) {auto o=fix(epoch+600,i);o.kind=Evidence::Unknown;p.observe(o,600000,epoch+600,true);}
    auto bad=fix(epoch+600,10);bad.lat=std::numeric_limits<double>::quiet_NaN();p.observe(bad,600000,epoch+600,true);
    bad=fix(epoch+600,11);bad.lon=181;p.observe(bad,600000,epoch+600,true);
    bad=fix(epoch+600,12);bad.quality_ok=false;p.observe(bad,600000,epoch+600,true);
    REQUIRE(!p.tick(900000).auto_home);
}
TEST(generation_vin_request_order_and_uninitialized_clock) {
    Policy p;p.configure(config(),2,0);p.observe(fix(epoch),0,epoch,true);REQUIRE(!p.tick(0).auto_home);
    auto o=fix(epoch,1,0,2);o.vin[0]='7';p.observe(o,0,epoch,true);REQUIRE(!p.tick(0).auto_home);
    o=fix(epoch,3,0,2);p.observe(o,0,epoch,false);REQUIRE(!p.tick(0).auto_home);
    o.request=4;p.observe(o,0,epoch,true);REQUIRE(p.tick(0).auto_home);
    p.observe(fix(epoch+10,2,1,2),10000,epoch+10,true);REQUIRE(p.tick(10000).auto_home);
    auto c=config();c.home_lat=1;p.configure(c,3,15000);p.observe(fix(epoch+16,5,0,2),16000,epoch+16,true);
    REQUIRE(!p.tick(16000).auto_home);
}
TEST(boundaries_and_antimeridian) {
    constexpr double radius=6371008.8,pi=3.141592653589793;
    REQUIRE(std::abs(distance_m(0,179.9999,0,-179.9999)-22.239)<0.01);
    REQUIRE(std::isfinite(distance_m(90,180,-90,0)));
    Policy p;p.configure(config(),1,0);
    p.observe(fix(epoch,1,(100.0-1e-6)/radius*180/pi),0,epoch,true);REQUIRE(p.tick(0).auto_home);
    p.observe(fix(epoch+1,2,(200.0+1e-6)/radius*180/pi),1000,epoch+1,true);REQUIRE(!p.tick(1000).auto_home);
}
TEST(monotonic_long_uptime_and_clock_jumps) {
    Ms boot=Ms(1)<<45;Policy p(boot);p.configure(config(),1,boot);p.observe(fix(epoch),boot,epoch,true);
    REQUIRE(p.tick(boot+30000).commanded);REQUIRE(!p.tick(boot+900000).commanded);
    ClockGuard c;REQUIRE(!c.update(0,100,false));REQUIRE(c.update(1000,epoch,true));
    REQUIRE(c.update(2000,epoch+1,true));REQUIRE(!c.update(3000,epoch+3600,true));REQUIRE(c.jumped());
    Policy q;q.configure(config(),1,0);q.observe(fix(epoch),0,epoch,true);REQUIRE(q.tick(30000).commanded);q.clock_discontinuity(30001);
    REQUIRE(!q.tick(30001).auto_home);
}
TEST(critical_fault_always_inhibits) {
    Policy p;p.configure(config(),1,0);p.timed_on(3600,0);REQUIRE(p.tick(30000).commanded);
    p.fault(30001);REQUIRE(!p.tick(30001).commanded);REQUIRE(!p.timed_on(3600,60000));
}
TEST(fake_gpio_trace_has_dwell_and_no_renewal_pulse) {
    struct FakeGPIO {
        bool on=false;
        std::vector<std::pair<Ms,bool>> edges{{0,false}};
        void command(Ms now,bool next) {if(next!=on) {on=next;edges.push_back({now,next});}}
    } gpio;
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    for(Ms now=0;now<=1500000;now+=50) {
        if(now==600000)p.observe(fix(epoch+600,2),now,epoch+600,true);
        gpio.command(now,p.tick(now).commanded);
    }
    REQUIRE(gpio.edges.size()==3);REQUIRE(gpio.edges[1].first==30000);REQUIRE(gpio.edges[1].second);
    REQUIRE(gpio.edges[2].first==1500000);REQUIRE(!gpio.edges[2].second);
}
TEST(sleep_after_known_departure_cannot_resurrect) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    p.observe(fix(epoch+60,2,1),60000,epoch+60,true);
    auto asleep=fix(epoch,3);asleep.kind=Evidence::Asleep;p.observe(asleep,120000,epoch+120,true);
    REQUIRE(!p.tick(120000).auto_home);REQUIRE(!p.tick(120000).commanded);
}
