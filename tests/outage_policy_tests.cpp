#include "test.hpp"
#include "protocol.hpp"
using namespace tsl;
namespace {
Config held_config() {
    auto c=config();c.outage_policy=uint8_t(OutagePolicy::HoldLast);return c;
}
}
TEST(held_home_survives_missing_reports_without_fabricating_freshness) {
    Policy p;p.configure(held_config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(30000).commanded);REQUIRE(!p.tick(30000).retained);
    auto d=p.tick(900000);
    REQUIRE(d.commanded && d.auto_home && d.retained);
    REQUIRE(d.lease_left==0 && d.reason==Reason::HomeRetained && d.last_source_s==epoch);
    const Ms week=Ms(7)*86400000;
    d=p.tick(week);REQUIRE(d.commanded && d.retained && d.last_source_s==epoch);
    auto unknown=fix(epoch+604800,2);unknown.kind=Evidence::Unknown;
    p.observe(unknown,week,epoch+604800,true);REQUIRE(p.tick(week).commanded);
    auto duplicate=fix(epoch,3);p.observe(duplicate,week,epoch+604800,true);
    REQUIRE(p.tick(week).retained && p.tick(week).lease_left==0);
}
TEST(held_home_requires_confirmed_away_and_does_not_hold_manual_on) {
    Policy p;p.configure(held_config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(900000).commanded);
    auto stale=fix(epoch+1,2,0.003);p.observe(stale,900000,epoch+900,true);
    REQUIRE(p.tick(900000).commanded);
    auto away=fix(epoch+901,3,0.003);p.observe(away,901000,epoch+901,true);
    REQUIRE(!p.tick(901000).auto_home && !p.tick(901000).commanded);
    auto sleep=fix(epoch+902,4);sleep.kind=Evidence::Asleep;
    p.observe(sleep,902000,epoch+902,true);REQUIRE(!p.tick(902000).commanded);
    REQUIRE(p.timed_on(60,902000));REQUIRE(!p.tick(902000).commanded);
    REQUIRE(p.tick(932000).commanded && !p.tick(932000).auto_home);
    REQUIRE(!p.tick(962000).commanded); // Timed ON is never the retained AUTO latch.
    p.observe(fix(epoch+963,5),963000,epoch+963,true);
    REQUIRE(!p.tick(963000).commanded);REQUIRE(p.tick(992000).commanded);
}
TEST(held_home_retains_hysteresis_after_expiry_but_band_cannot_create_home) {
    Policy p;p.configure(held_config(),1,0);
    p.observe(fix(epoch,1,0.00135),0,epoch,true);REQUIRE(!p.tick(30000).auto_home);
    p.observe(fix(epoch+31,2),31000,epoch+31,true);REQUIRE(p.tick(31000).commanded);
    p.observe(fix(epoch+1000,3,0.00135),1000000,epoch+1000,true);
    REQUIRE(p.tick(1000000).commanded);
    p.observe(fix(epoch+1001,4,0.003),1001000,epoch+1001,true);
    REQUIRE(!p.tick(1001000).auto_home);
    p.observe(fix(epoch+1002,5,0.00135),1002000,epoch+1002,true);
    REQUIRE(!p.tick(1031000).commanded);
}
TEST(held_home_off_revocation_and_fault_win_over_missing_data) {
    for(int cancel=0;cancel<3;++cancel) {
        Policy p;p.configure(held_config(),1,0);p.observe(fix(epoch),0,epoch,true);
        REQUIRE(p.tick(900000).commanded);
        if(cancel==0)p.off(2,900001);
        else if(cancel==1) {auto revoked=fix(epoch,2);revoked.kind=Evidence::Revoked;p.observe(revoked,900001,epoch+900,false);}
        else p.fault(900001);
        REQUIRE(!p.tick(900001).commanded && !p.tick(900001).auto_home);
        auto late=fix(epoch+901,1);p.observe(late,901000,epoch+901,true);
        REQUIRE(!p.tick(901000).commanded);
    }
}
TEST(held_home_clock_failure_never_changes_old_evidence_into_fresh_data) {
    Policy p;p.configure(held_config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(30000).commanded);p.clock_discontinuity(31000);
    REQUIRE(p.tick(31000).commanded && p.tick(31000).retained && p.tick(31000).lease_left==0);
    auto away=fix(epoch+32,2,0.003);p.observe(away,32000,epoch+32,false);
    REQUIRE(p.tick(32000).commanded);
    away.request=3;p.observe(away,33000,epoch+33,true);REQUIRE(!p.tick(33000).commanded);
}
TEST(held_missing_saved_state_and_settings_changes_require_new_home) {
    Policy before;before.configure(held_config(),1,0);before.observe(fix(epoch),0,epoch,true);
    REQUIRE(before.tick(900000).commanded);
    Policy reboot;reboot.configure(held_config(),2,0);
    auto asleep=fix(epoch,1,0,2);asleep.kind=Evidence::Asleep;
    reboot.observe(asleep,1000,epoch+1,true);REQUIRE(!reboot.tick(30000).auto_home);
    reboot.observe(fix(epoch+2,2,0,2),2000,epoch+2,true);
    REQUIRE(!reboot.tick(29999).commanded);REQUIRE(reboot.tick(30000).commanded);
    reboot.configure(held_config(),3,31000);REQUIRE(!reboot.tick(31000).commanded);
    reboot.observe(fix(epoch+32,3,0,2),32000,epoch+32,true);
    REQUIRE(!reboot.tick(62000).commanded);
    auto disabled=held_config();disabled.disabled=true;
    reboot.configure(disabled,4,63000);reboot.observe(fix(epoch+64,4,0,4),64000,epoch+64,true);
    REQUIRE(!reboot.tick(94000).commanded);
}
TEST(held_home_sleep_ceiling_does_not_cancel_explicit_retention_policy) {
    Policy p;p.configure(held_config(),1,0);p.observe(fix(epoch),0,epoch,true);
    auto asleep=fix(epoch,2);asleep.kind=Evidence::Asleep;
    p.observe(asleep,600000,epoch+600,true);REQUIRE(p.tick(600000).lease_left==900000);
    p.observe(asleep,90000000,epoch+90000,true);
    REQUIRE(p.tick(90000000).retained && p.tick(90000000).last_source_s==epoch);
    REQUIRE(p.tick(90000000).lease_left==0 && p.tick(90000000).commanded);
}
TEST(held_home_timed_expiry_has_no_pulse_and_output_gates_remain_enforced) {
    Policy p;p.configure(held_config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(900000).commanded && p.tick(900000).retained);
    REQUIRE(p.timed_on(60,900000));REQUIRE(p.tick(900000).timed);
    auto d=p.tick(960000);REQUIRE(d.commanded && d.retained && !d.timed);
    REQUIRE(d.reason==Reason::HomeRetained);
    REQUIRE(p.tick(Ms(90)*86400000).commanded); // Beyond the 32-bit milliseconds boundary.
    for(int gate=0;gate<2;++gate) {
        auto c=held_config();if(gate==0)c.commissioned=false;else c.dry_run=true;
        Policy blocked;blocked.configure(c,1,0);blocked.observe(fix(epoch),0,epoch,true);
        d=blocked.tick(900000);REQUIRE(d.auto_home && d.retained && !d.commanded);
    }
}
TEST(legacy_padding_never_opts_into_hold_and_schema_validation_is_explicit) {
    for(uint32_t version:{1U,2U})for(uint8_t padding:{uint8_t(0),uint8_t(1),uint8_t(255)}) {
        auto c=config();c.version=version;c.outage_policy=padding;
        REQUIRE(valid_config(c));REQUIRE(effective_outage_policy(c)==OutagePolicy::Expire);
        Policy p;p.configure(c,1,0);p.observe(fix(epoch),0,epoch,true);
        REQUIRE(!p.tick(900000).commanded);
        Json j;REQUIRE(j.parse(R"({"vin":"5YJ3E1EA7KF000001","home_lat":0,"home_lon":0})"));
        REQUIRE(parse_config(j,0,c));REQUIRE(c.version==3 && c.outage_policy==0);
    }
    for(const char* value:{"\"hold_last\"","\"expire\"","null","true","1","\"hold\""}) {
        auto c=config();Json j;
        auto wire=std::string(R"({"vin":"5YJ3E1EA7KF000001","home_lat":0,"home_lon":0,"outage_policy":)")+value+"}";
        REQUIRE(j.parse(wire));bool accepted=parse_config(j,0,c);
        REQUIRE(accepted==(std::string(value)=="\"hold_last\"" || std::string(value)=="\"expire\""));
    }
    auto c=config();c.outage_policy=2;REQUIRE(!valid_config(c));
}
