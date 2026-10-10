#include "test.hpp"
#include "firmware_update.hpp"
#include <array>
namespace {
std::array<uint8_t,4096> header() {
    std::array<uint8_t,4096> p{};
    p[0]=0xe9;p[1]=4;p[12]=9;
    p[32]=0x32;p[33]=0x54;p[34]=0xcd;p[35]=0xab;
    std::strcpy(reinterpret_cast<char*>(p.data()+48),"0.2.0");
    std::strcpy(reinterpret_cast<char*>(p.data()+80),"tsl_smart_contactor");
    return p;
}
tsl::FirmwareUpdate receiving() {
    tsl::FirmwareUpdate u;REQUIRE(u.begin(8192,1,0));REQUIRE(u.ready(50,1));return u;
}
}
TEST(firmware_requires_signed_sector_size_and_single_flight) {
    tsl::FirmwareUpdate u;
    REQUIRE(!u.begin(4096,1,0));REQUIRE(!u.begin(8193,1,0));
    REQUIRE(!u.begin(0x301000,1,0));REQUIRE(!u.begin(8192,0,0));
    REQUIRE(u.begin(8192,1,0));REQUIRE(!u.begin(8192,1,0));
    REQUIRE(!u.chunk(0,header().data(),4096,10,1));
}
TEST(firmware_accepts_only_correct_project_chip_and_version) {
    auto p=header();REQUIRE(tsl::compatible_firmware(p.data(),p.size()));
    REQUIRE(!tsl::compatible_firmware(nullptr,4096));REQUIRE(!tsl::compatible_firmware(p.data(),287));
    p[12]=0;REQUIRE(!tsl::compatible_firmware(p.data(),p.size()));p=header();
    p[80]='X';REQUIRE(!tsl::compatible_firmware(p.data(),p.size()));p=header();
    std::strcpy(reinterpret_cast<char*>(p.data()+48),"0.1.0");
    REQUIRE(!tsl::compatible_firmware(p.data(),p.size()));p=header();
    std::memset(p.data()+48,'2',32);REQUIRE(!tsl::compatible_firmware(p.data(),p.size()));
}
TEST(firmware_chunks_cannot_skip_replay_overlap_or_overflow) {
    auto u=receiving();auto p=header();
    REQUIRE(!u.chunk(1,p.data(),4096,100,1));REQUIRE(!u.chunk(0,p.data(),4097,100,1));
    REQUIRE(!u.chunk(0,p.data(),0,100,1));REQUIRE(!u.chunk(0,p.data(),4096,100,2));
    REQUIRE(u.chunk(0,p.data(),4096,100,1));REQUIRE(u.received()==0);
    REQUIRE(!u.chunk(0,p.data(),4096,100,1));REQUIRE(!u.written(1,150,1));
    REQUIRE(u.written(4096,150,1));REQUIRE(u.received()==4096);
    REQUIRE(!u.chunk(0,p.data(),4096,200,1));REQUIRE(!u.finish(200,1));
    REQUIRE(u.chunk(4096,p.data(),4096,200,1));REQUIRE(u.written(4096,250,1));
    REQUIRE(!u.chunk(8192,p.data(),1,300,1));REQUIRE(u.finish(300,1));
    REQUIRE(!u.verified(300,2));REQUIRE(u.verified(350,1));
    REQUIRE(u.state()==tsl::UpdateState::Rebooting);
}
TEST(firmware_idle_total_and_generation_cancellation) {
    auto u=receiving();REQUIRE(!u.current(30050,1));REQUIRE(!u.current(100,2));
    REQUIRE(!u.current(49,1));u.fail("synthetic_failure");REQUIRE(!u.active());
    REQUIRE(u.begin(8192,2,100));REQUIRE(!u.ready(180100,2));
    // Frequent writes never reset the total transfer deadline.
    tsl::FirmwareUpdate large;auto p=header();REQUIRE(large.begin(0x300000,1,0));
    REQUIRE(large.ready(1,1));
    for(size_t i=0;i<18;++i) {
        auto now=10000*tsl::Ms(i+1);
        bool ok=large.chunk(i*4096,p.data(),4096,now,1);
        if(i==17)REQUIRE(!ok);
        else {REQUIRE(ok);REQUIRE(large.written(4096,now,1));}
    }
}
TEST(update_gate_preserves_auto_and_observes_actual_off_dwell) {
    tsl::Policy p;p.configure(config(),1,0);p.observe(fix(epoch),30000,epoch,true);
    REQUIRE(p.tick(30000).commanded);
    auto d=p.tick(40000,true,true);REQUIRE(!d.commanded && d.desired && d.auto_home);
    REQUIRE(d.reason==tsl::Reason::FirmwareUpdate);
    REQUIRE(!p.tick(69999).commanded);REQUIRE(p.tick(70000).commanded);
    p.tick(80000,true,true);p.off(2,80001);
    REQUIRE(!p.tick(120000).desired);REQUIRE(!p.tick(120000).commanded);
}
TEST(update_gate_cannot_extend_expiry_or_override) {
    tsl::Policy p;p.configure(config(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(p.tick(30000).commanded);REQUIRE(!p.tick(900000,true,true).auto_home);
    REQUIRE(!p.tick(930000).commanded);
    REQUIRE(p.timed_on(60,940000));REQUIRE(!p.tick(1000000,true,true).timed);
    REQUIRE(!p.tick(1030000).commanded);
}
