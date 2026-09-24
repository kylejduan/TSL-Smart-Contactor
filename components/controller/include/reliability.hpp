#pragma once
#include "protocol.hpp"
#include <array>
#include <cstring>
namespace tsl {
enum class ReadResult { Ok, Missing, Failed };
struct Store {
    virtual ~Store() = default;
    virtual ReadResult read(const char* key, void* dst, size_t size) = 0;
    // Atomically replace one blob; true only after durable commit.
    virtual bool write(const char* key, const void* src, size_t size) = 0;
};
template<class T> void seal(T& r) { r.crc=crc32(&r,offsetof(T,crc)); }
template<class T> bool intact(const T& r) { return r.version==1 && r.crc==crc32(&r,offsetof(T,crc)); }
struct TokenRecord {
    uint32_t version = 1, sequence = 0;
    char current[2049] = {}, previous[2049] = {};
    int64_t uncertain_since = 0;
    uint32_t attempts = 0;
    uint8_t pending = 0, reauthorize = 0;
    uint32_t crc = 0;
};
class TokenJournal {
public:
    explicit TokenJournal(Store& s) : store_(s) {}
    Error load();
    Error provision(const char* refresh);
    Error begin(int64_t utc);
    Error finish(const char* replacement);
    Error revoke();
    void release() { busy_=false; }
    const char* current() const { return record_.current; }
    bool needs_reauth() const { return record_.reauthorize; }
private:
    bool save(TokenRecord& next);
    Store& store_;
    TokenRecord record_{};
    bool busy_ = false, loaded_ = false;
};
enum class Endpoint : uint8_t { Status, Location, Refresh, Count };
struct BudgetRecord {
    uint32_t version = 1, day = 0, month = 0;
    uint32_t daily[3] = {}, monthly[3] = {};
    uint32_t crc = 0;
};
class Budget {
public:
    explicit Budget(Store& s) : store_(s) {}
    Error load();
    Error take(Endpoint e, uint32_t utc_day, uint32_t utc_month, uint32_t day_cap, uint32_t month_cap);
    const BudgetRecord& counts() const { return record_; }
private:
    Store& store_;
    BudgetRecord record_{};
    uint8_t credit_[3] = {};
};
class Scheduler {
public:
    bool due(Ms now) const { return !busy_ && !paused_ && now>=next_; }
    bool begin(Ms now);
    void finish(Ms now, Error error, uint32_t poll_s, uint32_t retry_s, uint32_t random);
    bool check_now(Ms now);
    Ms next() const { return next_; }
    bool paused() const { return paused_; }
private:
    Ms next_=0, embargo_=0, manual_after_=0;
    unsigned failures_=0;
    bool busy_=false, paused_=false;
};
class Session {
public:
    bool login_allowed(Ms now) const { return now>=retry_; }
    void failed_login(Ms now);
    void establish(std::string_view cookie, std::string_view csrf, Ms now);
    bool authorized(std::string_view cookie, Ms now) const;
    bool change_allowed(std::string_view cookie, std::string_view csrf, Ms now) const;
    void clear();
    Ms remaining(Ms now) const {return expiry_>now ? expiry_-now : 0;}
private:
    char cookie_[65]={}, csrf_[65]={};
    Ms expiry_=0,retry_=0;
    unsigned failures_=0;
};
}
