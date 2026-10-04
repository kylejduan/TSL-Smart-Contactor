#include "test.hpp"
#include "control_epoch.hpp"
using namespace tsl;

TEST(settings_copied_before_off_cannot_acquire_a_new_generation) {
    ControlEpoch requests;Policy policy;auto saved=config();
    policy.configure(saved,requests.current(),0);
    policy.observe(fix(epoch),0,epoch,true);REQUIRE(policy.tick(30000).commanded);
    const auto observed=requests.current();const auto copied_settings=saved;
    // USB OFF completes while the web handler still holds an AUTO snapshot.
    auto off=requests.cancel();saved.disabled=true;policy.off(off,31000);
    uint32_t acquired=0;unsigned writes=0;
    if(requests.begin(observed,acquired)) {
        saved=copied_settings;++writes;policy.configure(saved,acquired,32000);
    }
    REQUIRE(writes==0);REQUIRE(saved.disabled);
    policy.observe(fix(epoch+33,2),33000,epoch+33,true);
    REQUIRE(!policy.tick(33000).auto_home);REQUIRE(!policy.tick(33000).commanded);
    REQUIRE(requests.current()==off);
}

TEST(delayed_authenticated_auto_rejects_off_but_fresh_auto_can_resume) {
    ControlEpoch requests;Policy policy;auto c=config();c.disabled=true;
    policy.configure(c,requests.current(),0);
    const auto before_password=requests.current();
    policy.off(requests.cancel(),10000);
    uint32_t acquired=0;
    REQUIRE(!requests.begin(before_password,acquired));
    REQUIRE(!policy.tick(30000).commanded);
    // A deliberate new command reads the latest status and may select AUTO.
    REQUIRE(requests.begin(requests.current(),acquired));
    c.disabled=false;policy.configure(c,acquired,30000);
    REQUIRE(!policy.tick(30000).commanded);
    policy.observe(fix(epoch+31,1,0,acquired),31000,epoch+31,true);
    REQUIRE(policy.tick(31000).commanded);
}

TEST(off_invalidates_already_acquired_config_and_timed_requests) {
    ControlEpoch requests;Policy policy;auto c=config();
    policy.configure(c,requests.current(),0);
    uint32_t in_flight=0;REQUIRE(requests.begin(requests.current(),in_flight));
    const auto off=requests.cancel();policy.off(off,10000);
    REQUIRE(!requests.matches(in_flight)); // writer cannot publish after OFF
    uint32_t fresh=0;REQUIRE(requests.begin(off,fresh));
    policy.configure(c,fresh,11000);
    bool stale_override=requests.matches(in_flight) && policy.timed_on(3600,12000);
    REQUIRE(!stale_override);REQUIRE(!policy.tick(30000).timed);
    REQUIRE(!policy.tick(30000).commanded);
    REQUIRE(requests.matches(fresh) && policy.timed_on(60,30000));
    REQUIRE(policy.tick(30000).commanded);REQUIRE(!policy.tick(30000).auto_home);
}

TEST(boot_generation_rejects_previous_boot_and_zero_marker) {
    ControlEpoch previous(1700),reboot(3000);uint32_t acquired=0;
    REQUIRE(!reboot.begin(previous.current(),acquired));
    REQUIRE(!reboot.begin(0,acquired));
    REQUIRE(reboot.begin(3000,acquired));REQUIRE(acquired==3001);
}
