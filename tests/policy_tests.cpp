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
TEST(reconnect_waits_for_utc_without_restarting_or_extending_a_lease) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(30000).commanded);
    // The link can be unavailable while the already-granted monotonic lease runs.
    p.observe(fix(epoch+600,2),600000,epoch+600,false);
    REQUIRE(p.tick(899999).commanded);
    REQUIRE(!p.tick(900000).auto_home);
    p.observe(fix(epoch+901,3),901000,epoch+901,false);
    REQUIRE(!p.tick(901000).auto_home);
    p.observe(fix(epoch+902,4),902000,epoch+902,true);
    REQUIRE(p.tick(902000).auto_home);
    REQUIRE(!p.tick(929999).commanded);
    REQUIRE(p.tick(930000).commanded);
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
TEST(report_basis_requires_explicit_current_online_evidence) {
    auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    Policy p;p.configure(c,1,0);
    p.observe(fix(epoch),0,epoch,true);REQUIRE(!p.tick(30000).auto_home);
    auto report=fix(epoch+31,2);report.position_basis=PositionBasis::VehicleReport;
    report.vehicle=Vehicle::Offline;p.observe(report,31000,epoch+31,true);
    REQUIRE(!p.tick(31000).auto_home);
    report.request=3;report.vehicle=Vehicle::Online;report.generation=2;
    p.observe(report,31000,epoch+31,true);REQUIRE(!p.tick(31000).auto_home);
    report.request=4;report.generation=1;p.observe(report,31000,epoch+31,true);
    REQUIRE(p.tick(31000).commanded);
    c.position_basis=uint8_t(PositionBasis::GpsSource);p.configure(c,2,32000);
    report.request=5;report.generation=2;report.source_s=epoch+32;
    p.observe(report,32000,epoch+32,true);REQUIRE(!p.tick(32000).auto_home);
}
TEST(report_duplicates_stale_and_future_evidence_do_not_renew) {
    auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    Policy p;p.configure(c,1,0);
    auto report=fix(epoch);report.position_basis=PositionBasis::VehicleReport;
    p.observe(report,0,epoch,true);REQUIRE(p.tick(30000).commanded);
    report.request=2;p.observe(report,600000,epoch+600,true); // duplicate
    report.request=3;report.source_s=epoch+479;p.observe(report,600000,epoch+600,true);
    report.request=4;report.source_s=epoch+631;p.observe(report,600000,epoch+600,true);
    REQUIRE(p.tick(899999).commanded);REQUIRE(!p.tick(900000).commanded);
}
TEST(report_sleep_ceiling_remains_anchored_to_last_report_not_sleep_receipts) {
    auto c=config();c.position_basis=uint8_t(PositionBasis::VehicleReport);
    Policy p;p.configure(c,1,0);
    auto report=fix(epoch);report.position_basis=PositionBasis::VehicleReport;
    p.observe(report,0,epoch,true);
    auto sleep=report;sleep.kind=Evidence::Asleep;sleep.vehicle=Vehicle::Asleep;
    for(int s=600;s<86400;s+=600) {
        sleep.request=2+s/600;p.observe(sleep,Ms(s)*1000,epoch+s,true);
        auto d=p.tick(Ms(s)*1000);REQUIRE(d.auto_home);
        REQUIRE(d.last_source_s==epoch);REQUIRE(d.lease_left<=86400000-Ms(s)*1000);
    }
    REQUIRE(!p.tick(86400000).auto_home);
    sleep.request=1000;p.observe(sleep,86400001,epoch+86400,true);
    REQUIRE(!p.tick(86400001).auto_home);
}
TEST(version_one_padding_cannot_opt_into_report_policy) {
    for(uint8_t padding:{uint8_t(1),uint8_t(255)}) {
        auto c=config();c.version=1;c.position_basis=padding;
        REQUIRE(valid_config(c));REQUIRE(effective_position_basis(c)==PositionBasis::GpsSource);
        Policy p;p.configure(c,1,0);
        auto report=fix(epoch);report.position_basis=PositionBasis::VehicleReport;
        p.observe(report,0,epoch,true);REQUIRE(!p.tick(30000).auto_home);
        p.observe(fix(epoch+31,2),31000,epoch+31,true);REQUIRE(p.tick(31000).commanded);
    }
    auto c=config();c.position_basis=2;REQUIRE(!valid_config(c));
    c=config();c.version=3;REQUIRE(!valid_config(c));
}
TEST(repeated_week_long_outages_recover_across_32bit_millisecond_boundary) {
    const Ms boot=Ms(std::numeric_limits<uint32_t>::max())-600000;
    constexpr int64_t week_s=7*86400;
    Policy p(boot);p.configure(config(),1,boot);
    ClockGuard clock;uint64_t request=0;
    // Eight recoveries span 49 days, including the common 32-bit uptime boundary.
    // Only fresh GPS on a usable clock may recover AUTO after each expired lease.
    for(int cycle=0;cycle<8;++cycle) {
        const auto utc=epoch+cycle*week_s;
        const Ms now=boot+Ms(cycle)*week_s*1000;
        REQUIRE(clock.update(now,utc,true));REQUIRE(!clock.jumped());
        p.observe(fix(utc,++request),now,utc,true);
        REQUIRE(p.tick(now).auto_home);REQUIRE(p.tick(now).lease_left==900000);
        if(cycle==0)REQUIRE(!p.tick(now+29999).commanded);
        REQUIRE(p.tick(now+30000).commanded);
        // A duplicate, a failed poll, and a fresh-looking reply before time
        // readiness returns must not replace the original monotonic deadline.
        p.observe(fix(utc,++request),now+600000,utc+600,true);
        auto unavailable=fix(utc+700,++request);unavailable.kind=Evidence::Unknown;
        unavailable.vehicle=Vehicle::Offline;p.observe(unavailable,now+700000,utc+700,true);
        p.observe(fix(utc+800,++request),now+800000,utc+800,false);
        REQUIRE(p.tick(now+899999).commanded);
        REQUIRE(!p.tick(now+900000).commanded);
        auto asleep=fix(utc,++request);asleep.kind=Evidence::Asleep;asleep.vehicle=Vehicle::Asleep;
        p.observe(asleep,now+901000,utc+901,true);
        REQUIRE(!p.tick(now+901000).auto_home);
        p.observe(fix(utc+6*86400,++request),now+Ms(6)*86400000,utc+6*86400,false);
        REQUIRE(!p.tick(now+Ms(6)*86400000).commanded);
        REQUIRE(p.tick(now+Ms(6)*86400000).last_source_s==utc);
    }
}
TEST(power_loss_restarts_off_and_does_not_restore_sleep_or_override_authorization) {
    const auto persisted=config();
    for(uint32_t boot=1;boot<=4;++boot) {
        const int64_t utc=epoch+int64_t(boot)*86400;
        Policy p;p.configure(persisted,boot,0);
        // A successful sleeping response after reboot is not location evidence.
        auto asleep=fix(utc-60,1,0,boot);asleep.kind=Evidence::Asleep;asleep.vehicle=Vehicle::Asleep;
        p.observe(asleep,1000,utc+1,true);
        REQUIRE(!p.tick(1000).auto_home);REQUIRE(!p.tick(1000).timed);
        REQUIRE(!p.tick(1000).commanded);
        p.observe(fix(utc+2,2,0,boot),2000,utc+2,true);
        REQUIRE(p.tick(2000).auto_home);REQUIRE(!p.tick(29999).commanded);
        REQUIRE(p.tick(30000).commanded);
        REQUIRE(p.timed_on(28800,30000));REQUIRE(p.tick(30000).timed);
        // Only the configuration is carried to the next simulated power-up.
    }
    auto disabled=persisted;disabled.disabled=true;
    Policy reboot;reboot.configure(disabled,9,0);
    reboot.observe(fix(epoch+5*86400,1,0,9),30000,epoch+5*86400,true);
    REQUIRE(reboot.tick(30000).reason==Reason::Disabled);
    REQUIRE(!reboot.tick(30000).auto_home);REQUIRE(!reboot.timed_on(3600,30000));
    auto uncommissioned=persisted;uncommissioned.commissioned=false;
    Policy bench;bench.configure(uncommissioned,10,0);
    bench.observe(fix(epoch+6*86400,1,0,10),30000,epoch+6*86400,true);
    REQUIRE(bench.tick(30000).reason==Reason::Uncommissioned);
    REQUIRE(!bench.tick(30000).commanded);REQUIRE(!bench.timed_on(3600,30000));
}
TEST(clock_reacquisition_and_jumps_never_extend_a_running_override) {
    Policy p;p.configure(config(),1,0);ClockGuard clock;
    REQUIRE(clock.update(0,epoch,true));p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.timed_on(3600,0));REQUIRE(p.tick(30000).commanded);
    REQUIRE(!clock.update(600000,epoch+600,false));REQUIRE(clock.jumped());
    p.clock_discontinuity(600000);
    REQUIRE(!p.tick(600000).auto_home);REQUIRE(p.tick(600000).override_left==3000000);
    REQUIRE(clock.update(601000,epoch+601,true));
    auto asleep=fix(epoch,2);asleep.kind=Evidence::Asleep;asleep.vehicle=Vehicle::Asleep;
    p.observe(asleep,601000,epoch+601,true);REQUIRE(!p.tick(601000).auto_home);
    p.observe(fix(epoch+602,3),602000,epoch+602,true);
    REQUIRE(p.tick(602000).auto_home);
    // A large forward correction and its reversal each invalidate AUTO;
    // neither changes the explicit override's monotonic expiry at t=3600.
    REQUIRE(!clock.update(603000,epoch+4203,true));REQUIRE(clock.jumped());
    p.clock_discontinuity(603000);REQUIRE(!p.tick(603000).auto_home);
    REQUIRE(clock.update(604000,epoch+4204,true));
    asleep.request=4;p.observe(asleep,604000,epoch+4204,true);
    REQUIRE(!p.tick(604000).auto_home);
    REQUIRE(!clock.update(605000,epoch+605,true));REQUIRE(clock.jumped());
    p.clock_discontinuity(605000);
    REQUIRE(p.tick(605000).override_left==2995000);
    REQUIRE(p.tick(3599999).commanded);REQUIRE(!p.tick(3600000).commanded);
    REQUIRE(!p.tick(3600000).timed);REQUIRE(!p.tick(3600000).auto_home);
}
TEST(critical_fault_cannot_be_cleared_by_configuration_or_new_evidence) {
    Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(30000).commanded);p.fault(31000);
    p.configure(config(),2,32000);
    p.observe(fix(epoch+33,1,0,2),33000,epoch+33,true);
    REQUIRE(p.tick(33000).reason==Reason::Fault);
    REQUIRE(!p.tick(33000).auto_home);REQUIRE(!p.timed_on(3600,33000));
    REQUIRE(!p.tick(86400000).commanded);
    // Reboot may recover a transient local failure, but authorization still
    // starts empty and requires fresh evidence plus the complete boot dwell.
    Policy reboot;reboot.configure(config(),3,0);
    REQUIRE(!reboot.tick(0).auto_home);
    reboot.observe(fix(epoch+86400,1,0,3),0,epoch+86400,true);
    REQUIRE(!reboot.tick(29999).commanded);REQUIRE(reboot.tick(30000).commanded);
}
TEST(ntp_only_outage_bounds_new_evidence_without_extending_or_canceling_existing_deadlines) {
    for(uint32_t sync:{uint32_t(0),std::numeric_limits<uint32_t>::max()-1000}) {
        const Ms boot=Ms(sync)*1000;
        auto now=[&](uint32_t elapsed) {return boot+Ms(elapsed)*1000;};
        auto ready=[&](uint32_t elapsed,uint32_t last_sync,bool synchronized=true) {
            return recent_utc_sync(uint32_t(now(elapsed)/1000),last_sync,synchronized);
        };
        Policy p(boot);p.configure(config(),1,boot);
        const auto home_at=kUtcSyncMaxAgeS-1,stale_at=kUtcSyncMaxAgeS+1;
        REQUIRE(!ready(0,sync,false));REQUIRE(ready(kUtcSyncMaxAgeS,sync));
        p.observe(fix(epoch+home_at),now(home_at),epoch+home_at,ready(home_at,sync));
        REQUIRE(p.tick(now(home_at)).commanded);
        REQUIRE(!ready(stale_at,sync));
        p.observe(fix(epoch+stale_at,2),now(stale_at),epoch+stale_at,ready(stale_at,sync));
        REQUIRE(p.tick(now(stale_at)).lease_left==898000);
        auto asleep=fix(epoch+stale_at,3);asleep.kind=Evidence::Asleep;asleep.vehicle=Vehicle::Asleep;
        p.observe(asleep,now(stale_at),epoch+stale_at,ready(stale_at,sync));
        REQUIRE(p.tick(now(stale_at)).lease_left==898000);
        const auto expired_at=home_at+900;
        REQUIRE(p.tick(now(expired_at)-1).commanded);
        REQUIRE(!p.tick(now(expired_at)).commanded);
        const auto resync_at=expired_at+1;
        const auto new_sync=uint32_t(now(resync_at)/1000);
        REQUIRE(ready(resync_at,new_sync));
        p.observe(fix(epoch+resync_at,4),now(resync_at),epoch+resync_at,ready(resync_at,new_sync));
        REQUIRE(p.tick(now(resync_at)).auto_home);
        REQUIRE(!p.tick(now(expired_at+30)-1).commanded);
        REQUIRE(p.tick(now(expired_at+30)).commanded);

        Policy manual(boot);manual.configure(config(),1,boot);
        REQUIRE(manual.timed_on(3600,now(home_at)));
        manual.observe(fix(epoch+stale_at),now(stale_at),epoch+stale_at,ready(stale_at,sync));
        REQUIRE(!manual.tick(now(stale_at)).auto_home);
        REQUIRE(manual.tick(now(stale_at)).override_left==3598000);
        REQUIRE(manual.tick(now(home_at+3600)-1).commanded);
        REQUIRE(!manual.tick(now(home_at+3600)).commanded);
    }
}
