#include "test.hpp"
#include "reliability.hpp"
#include <map>
using namespace tsl;
struct Memory : Store {
    std::map<std::string,std::vector<uint8_t>> records;
    bool fail=false,commit_then_fail=false;
    ReadResult read(const char* key,void* out,size_t n) override {
        auto i=records.find(key);if(i==records.end())return ReadResult::Missing;
        if(i->second.size()!=n)return ReadResult::Failed;
        std::memcpy(out,i->second.data(),n);return ReadResult::Ok;
    }
    bool write(const char* key,const void* p,size_t n) override {
        if(fail)return false;
        const auto* b=static_cast<const uint8_t*>(p);records[key]={b,b+n};
        return !commit_then_fail;
    }
};
TEST(refresh_serialized_and_committed_before_handoff) {
    Memory store;TokenJournal j(store);REQUIRE(j.provision("synthetic-old")==Error::None);
    REQUIRE(j.begin(epoch)==Error::None);REQUIRE(j.begin(epoch)==Error::Authentication);
    REQUIRE(j.finish("synthetic-new")==Error::None);
    TokenJournal restarted(store);REQUIRE(restarted.load()==Error::None);
    REQUIRE(std::string(restarted.current())=="synthetic-new");
}
TEST(power_loss_before_during_and_after_rotation_commit) {
    Memory store;TokenJournal j(store);j.provision("old");
    store.fail=true;REQUIRE(j.begin(epoch)==Error::Storage);store.fail=false;
    TokenJournal before(store);REQUIRE(before.load()==Error::None);REQUIRE(std::string(before.current())=="old");
    REQUIRE(before.begin(epoch)==Error::None); // server rotates, client loses response
    TokenJournal lost(store);REQUIRE(lost.load()==Error::None);REQUIRE(lost.begin(epoch+60)==Error::None);
    store.fail=true;REQUIRE(lost.finish("new")==Error::Storage);store.fail=false;
    TokenJournal during(store);REQUIRE(during.load()==Error::None);REQUIRE(std::string(during.current())=="old");
    REQUIRE(during.begin(epoch+120)==Error::None);store.commit_then_fail=true;
    REQUIRE(during.finish("new")==Error::Storage);store.commit_then_fail=false;
    TokenJournal after(store);REQUIRE(after.load()==Error::None);REQUIRE(std::string(after.current())=="new");
}
TEST(lost_refresh_response_recovery_is_bounded_and_corruption_explicit) {
    Memory store;TokenJournal j(store);j.provision("old");
    for(int i=0;i<3;++i) {TokenJournal r(store);REQUIRE(r.load()==Error::None);REQUIRE(r.begin(epoch+i*100)==Error::None);}
    TokenJournal exhausted(store);REQUIRE(exhausted.load()==Error::None);REQUIRE(exhausted.begin(epoch+300)==Error::Reauthorize);
    TokenJournal revoked(store);REQUIRE(revoked.load()==Error::Reauthorize);
    store.records["tokens"][8]^=1;TokenJournal corrupt(store);REQUIRE(corrupt.load()==Error::Storage);
    Memory other;TokenJournal timeout(other);timeout.provision("old");timeout.begin(epoch);timeout.release();
    REQUIRE(timeout.begin(epoch+86400)==Error::Reauthorize);
}
TEST(budget_is_conservative_across_reboot_and_commit_failures) {
    Memory store;Budget b(store);REQUIRE(b.load()==Error::None);
    REQUIRE(b.take(Endpoint::Status,100,20,5,10)==Error::None);
    Budget reboot(store);REQUIRE(reboot.load()==Error::None);
    REQUIRE(reboot.take(Endpoint::Location,100,20,5,10)==Error::None);
    REQUIRE(reboot.take(Endpoint::Location,100,20,5,10)==Error::Budget);
    REQUIRE(reboot.take(Endpoint::Location,99,20,5,10)==Error::Clock);
    store.fail=true;REQUIRE(reboot.take(Endpoint::Refresh,101,20,5,10)==Error::Storage);
}
TEST(monthly_data_allowance_limits_location_reservations_across_reboot) {
    Memory store;Budget budget(store);REQUIRE(budget.load()==Error::None);
    for(uint32_t i=0;i<kMonthlyDataRequestCap-3;++i)
        REQUIRE(budget.take(Endpoint::Location,100,20,10000,12000)==Error::None);
    REQUIRE(budget.counts().monthly[1]==kMonthlyDataRequestCap);
    Budget reboot(store);REQUIRE(reboot.load()==Error::None);
    REQUIRE(reboot.take(Endpoint::Location,100,20,10000,12000)==Error::Budget);
    REQUIRE(reboot.take(Endpoint::Status,100,20,10000,12000)==Error::None);
    REQUIRE(reboot.take(Endpoint::Location,101,21,10000,12000)==Error::None);
}
TEST(scheduler_single_flight_manual_backoff_retryafter_and_permanent_pause) {
    Scheduler s;REQUIRE(s.begin(0));REQUIRE(!s.begin(0));REQUIRE(!s.check_now(0));
    s.finish(10,Error::RateLimit,600,500,0);REQUIRE(!s.due(500009));REQUIRE(!s.check_now(60000));
    REQUIRE(s.begin(500010));s.finish(500020,Error::Permission,600,0,0);REQUIRE(s.paused());
    REQUIRE(!s.due(9000000));REQUIRE(s.check_now(9000000));REQUIRE(s.begin(9000000));
    s.finish(9000001,Error::None,600,0,0);REQUIRE(!s.due(9600000));REQUIRE(s.due(9600001));
}
TEST(manual_check_has_ten_minute_cooldown) {
    Scheduler s;REQUIRE(s.check_now(0));REQUIRE(s.begin(0));
    s.finish(1,Error::None,600,0,0);
    REQUIRE(!s.check_now(599999));REQUIRE(s.check_now(600000));
}
TEST(session_requires_auth_csrf_expiry_and_login_throttle) {
    Session s;std::string id(64,'a'),csrf(64,'b');
    REQUIRE(!s.authorized(id,0));s.establish(id,csrf,0);REQUIRE(s.authorized(id,1));
    REQUIRE(!s.change_allowed(id,"",1));REQUIRE(!s.change_allowed(id,std::string(64,'c'),1));
    REQUIRE(s.change_allowed(id,csrf,1));REQUIRE(!s.authorized(id,900000));
    s.failed_login(0);REQUIRE(!s.login_allowed(1000));REQUIRE(s.login_allowed(2000));
    s.clear();REQUIRE(!s.authorized(id,1));
}
TEST(lowered_caps_do_not_spend_old_reservations) {
    Memory store;Budget budget(store);budget.load();
    REQUIRE(budget.take(Endpoint::Location,100,20,20,40)==Error::None);
    REQUIRE(budget.take(Endpoint::Location,100,20,1,40)==Error::Budget);
}

#include "events.hpp"
TEST(event_history_retains_newest_sixteen_without_changing_copied_snapshots) {
    EventLog log;REQUIRE(log.size()==0);REQUIRE(log.newest(0)==nullptr);
    for(int i=0;i<20;++i)log.record(Ms(i)*1000,Reason::Disabled,i%2);
    REQUIRE(log.size()==16);REQUIRE(log.newest(0)->at==19000);REQUIRE(log.newest(15)->at==4000);
    REQUIRE(log.newest(16)==nullptr);auto copy=log;
    log.record(20000,Reason::Fault,false);
    REQUIRE(copy.newest(0)->at==19000);REQUIRE(log.newest(0)->reason==Reason::Fault);
    REQUIRE(!log.newest(0)->commanded);
}
