#include "test.hpp"
#include "auto_journal.hpp"
#include <map>
using namespace tsl;
namespace {
struct Memory:Store {
    std::map<std::string,std::vector<uint8_t>> blobs;
    unsigned writes=0;
    bool fail_before=false,fail_after=false,read_failure=false;
    ReadResult read(const char* key,void* out,size_t size) override {
        if(read_failure)return ReadResult::Failed;
        auto found=blobs.find(key);if(found==blobs.end())return ReadResult::Missing;
        if(found->second.size()!=size)return ReadResult::Failed;
        std::memcpy(out,found->second.data(),size);return ReadResult::Ok;
    }
    bool write(const char* key,const void* input,size_t size) override {
        ++writes;if(fail_before)return false;
        const auto* data=static_cast<const uint8_t*>(input);
        blobs[key]={data,data+size};return !fail_after;
    }
};
Config held() {auto c=config();c.outage_policy=uint8_t(OutagePolicy::HoldLast);return c;}
}
TEST(auto_journal_missing_record_is_unknown_and_identical_decisions_do_not_write) {
    Memory memory;AutoJournal journal(memory);AutoState state=AutoState::Home;
    REQUIRE(journal.load(held(),state)==Error::None && state==AutoState::Unknown);
    REQUIRE(journal.save(held(),AutoState::Home)==Error::None);
    for(int i=0;i<1000;++i)REQUIRE(journal.save(held(),AutoState::Home)==Error::None);
    REQUIRE(memory.writes==1);
    REQUIRE(journal.load(held(),state)==Error::None && state==AutoState::Home);
    REQUIRE(journal.save(held(),AutoState::Away)==Error::None && memory.writes==2);
    REQUIRE(journal.load(held(),state)==Error::None && state==AutoState::Away);
}
TEST(auto_journal_binds_all_settings_and_never_restores_legacy_or_disabled_home) {
    Memory memory;AutoJournal journal(memory);auto c=held();AutoState state;
    REQUIRE(journal.save(c,AutoState::Home)==Error::None);
    for(int change=0;change<8;++change) {
        auto changed=c;
        switch(change) {
        case 0:changed.vin[16]='2';break;
        case 1:changed.home_lon=0.1;break;
        case 2:changed.position_basis=1;break;
        case 3:changed.outage_policy=0;break;
        case 4:changed.disabled=true;break;
        case 5:changed.commissioned=false;break;
        case 6:changed.version=2;break;
        case 7:changed.poll_s=601;break;
        }
        REQUIRE(journal.load(changed,state)==Error::None && state==AutoState::Unknown);
    }
    c.disabled=true;REQUIRE(journal.save(c,AutoState::Home)==Error::None);
    REQUIRE(journal.load(c,state)==Error::None && state==AutoState::Unknown);
}
TEST(auto_journal_off_tombstone_prevents_reverting_to_an_old_matching_config) {
    Memory memory;AutoJournal journal(memory);AutoState state;
    REQUIRE(journal.save(held(),AutoState::Home)==Error::None);
    REQUIRE(journal.clear()==Error::None);
    REQUIRE(journal.load(held(),state)==Error::None && state==AutoState::Unknown);
    REQUIRE(journal.clear()==Error::None && memory.writes==2);
}
TEST(auto_journal_power_loss_retains_old_or_new_complete_decision) {
    for(bool after:{false,true}) {
        Memory memory;AutoJournal journal(memory);AutoState state;
        REQUIRE(journal.save(held(),AutoState::Home)==Error::None);
        memory.fail_before=!after;memory.fail_after=after;
        REQUIRE(journal.save(held(),AutoState::Away)==Error::Storage);
        AutoJournal reboot(memory);
        REQUIRE(reboot.load(held(),state)==Error::None);
        REQUIRE(state==(after ? AutoState::Away : AutoState::Home));
        // An interrupted OFF clear has the same old/new atomic boundary.
        memory.fail_before=false;memory.fail_after=false;
        REQUIRE(reboot.clear()==Error::None);
        REQUIRE(reboot.load(held(),state)==Error::None && state==AutoState::Unknown);
    }
}
TEST(auto_journal_corruption_wrong_size_read_failure_and_invalid_enum_fail_explicitly) {
    for(int corruption=0;corruption<6;++corruption) {
        Memory memory;AutoJournal journal(memory);AutoState state=AutoState::Home;
        REQUIRE(journal.save(held(),AutoState::Home)==Error::None);
        auto& blob=memory.blobs["auto_state"];
        if(corruption==0)blob[0]^=1;
        else if(corruption==1)blob.resize(3);
        else if(corruption==2)memory.read_failure=true;
        else {
            AutoRecord record;std::memcpy(&record,blob.data(),sizeof record);
            if(corruption==3)record.state=static_cast<AutoState>(3);
            if(corruption==4)record.reserved[1]=1;
            if(corruption==5)record.version=2;
            seal(record);std::memcpy(blob.data(),&record,sizeof record);
        }
        REQUIRE(journal.load(held(),state)==Error::Storage && state==AutoState::Unknown);
        REQUIRE(journal.clear()==Error::Storage);REQUIRE(memory.writes==1);
    }
}
TEST(restored_home_has_no_freshness_or_override_and_respects_dwell_and_away) {
    Policy p;p.configure(held(),2,0);p.restore(AutoState::Home);
    auto d=p.tick(29999);REQUIRE(d.auto_home && d.restored && d.retained && !d.commanded);
    d=p.tick(30000);REQUIRE(d.commanded && d.last_source_s==0 && d.distance==-1 && !d.timed);
    auto away=fix(epoch,1,0.003,2);p.observe(away,31000,epoch,true);
    d=p.tick(31000);REQUIRE(!d.commanded && !d.restored && d.auto_state==AutoState::Away);
    p.configure(held(),3,32000);p.restore(AutoState::Away);
    REQUIRE(!p.tick(62000).auto_home);
}
TEST(restoration_cannot_bypass_expiry_disabled_commissioning_or_local_fault) {
    for(int gate=0;gate<4;++gate) {
        auto c=held();if(gate==0)c.outage_policy=0;
        if(gate==1)c.disabled=true;
        if(gate==2)c.commissioned=false;
        Policy p;p.configure(c,1,0);if(gate==3)p.fault(0);
        p.restore(AutoState::Home);REQUIRE(!p.tick(30000).auto_home && !p.tick(30000).commanded);
    }
}
TEST(home_commit_gate_cannot_hide_off_dwell_or_save_manual_override) {
    Policy p;p.configure(held(),1,0);p.observe(fix(epoch),0,epoch,true);
    REQUIRE(!p.tick(30000,false).commanded && p.tick(30000,false).reason==Reason::HomePending);
    REQUIRE(p.tick(31000,true).commanded);
    p.observe(fix(epoch+32,2,0.003),32000,epoch+32,true);REQUIRE(!p.tick(32000,false).commanded);
    p.observe(fix(epoch+33,3),33000,epoch+33,true);
    REQUIRE(!p.tick(61000,true).commanded);REQUIRE(p.tick(62000,true).commanded);
    Policy manual;manual.configure(held(),1,0);REQUIRE(manual.timed_on(60,0));
    auto d=manual.tick(30000,false);REQUIRE(d.commanded && d.timed && d.auto_state==AutoState::Unknown);
    Policy reboot;reboot.configure(held(),1,0);reboot.restore(d.auto_state);
    REQUIRE(!reboot.tick(30000).commanded && !reboot.tick(30000).timed);
}
TEST(restored_away_rejects_older_and_duplicate_home_after_power_recovery) {
    Memory memory;AutoJournal journal(memory);AutoState state;int64_t source;
    REQUIRE(journal.save(held(),AutoState::Away,epoch+30)==Error::None);
    REQUIRE(journal.load(held(),state,&source)==Error::None && source==epoch+30);
    Policy p;p.configure(held(),2,0);p.restore(state,source);
    p.observe(fix(epoch+29,1,0,2),30000,epoch+31,true);
    p.observe(fix(epoch+30,2,0,2),30000,epoch+31,true);
    REQUIRE(!p.tick(30000).auto_home);
    p.observe(fix(epoch+31,3,0,2),30000,epoch+31,true);REQUIRE(p.tick(30000).commanded);
}
TEST(boot_recovery_checks_token_revocation_corruption_and_reset_permission_offline) {
    for(int condition=0;condition<5;++condition) {
        Memory memory;AutoJournal journal(memory);TokenJournal tokens(memory);
        REQUIRE(journal.save(held(),AutoState::Home,epoch)==Error::None);
        if(condition!=1)REQUIRE(tokens.provision("synthetic-refresh")==Error::None);
        if(condition==2)REQUIRE(tokens.revoke()==Error::Reauthorize);
        if(condition==3)memory.blobs["tokens"][0]^=1;
        AutoState state;int64_t source=0;
        auto result=recover_auto_state(memory,held(),condition!=4,state,source);
        REQUIRE(result==(condition==3 ? Error::Storage : Error::None));
        REQUIRE(state==(condition==0 ? AutoState::Home : AutoState::Unknown));
        if(condition!=0 && condition!=3) {
            REQUIRE(journal.load(held(),state)==Error::None && state==AutoState::Unknown);
        }
    }
}
TEST(config_decision_binding_ignores_padding_but_checks_every_setting) {
    auto c=held(),copy=c;
    reinterpret_cast<uint8_t*>(&copy)[86]=0xA5;
    REQUIRE(auto_config_crc(copy)==auto_config_crc(c));
    copy.dry_run=true;REQUIRE(auto_config_crc(copy)!=auto_config_crc(c));
}
