#pragma once
#include <cstdint>

namespace tsl {
using Ms = int64_t;
struct Config {
    uint32_t version = 1;
    char vin[18] = {};
    double home_lat = 0, home_lon = 0;
    uint32_t enable_m = 100, disable_m = 200;
    uint32_t max_age_s = 120, future_s = 30, lease_s = 900;
    uint32_t sleep_s = 86400, poll_s = 600, dwell_s = 30;
    uint32_t daily_cap = 400, monthly_cap = 12000;
    bool commissioned = false, disabled = true, dry_run = true;
    uint8_t region = 0; // 0 NA, 1 EU; no arbitrary URL
};
bool valid_config(const Config& c);
bool valid_vin(const char* vin);
double distance_m(double lat1, double lon1, double lat2, double lon2);

enum class Vehicle : uint8_t { Unknown, Online, Asleep, Offline };
enum class Evidence : uint8_t { Unknown, Location, Asleep, Revoked };
struct Observation {
    uint32_t generation = 0;
    uint64_t request = 0;
    char vin[18] = {};
    Evidence kind = Evidence::Unknown;
    Vehicle vehicle = Vehicle::Unknown;
    double lat = 0, lon = 0;
    int64_t source_s = 0;
    bool quality_ok = true;
};
enum class Reason : uint8_t {
    Uncommissioned, Disabled, Fault, NoAuthorization, Home, Timed,
    OffDwell, DryRun, ClockInvalid
};
const char* reason_name(Reason r);
const char* vehicle_name(Vehicle v);
struct Decision {
    bool auto_home = false, desired = false, commanded = false;
    bool timed = false;
    Ms lease_left = 0, override_left = 0;
    Reason reason = Reason::NoAuthorization;
    double distance = -1;
    int64_t last_source_s = 0;
};
// No I/O, allocations, persisted leases or wall-clock deadlines.
class Policy {
public:
    explicit Policy(Ms boot = 0) : off_since_(boot) {}
    void configure(const Config&, uint32_t generation, Ms now);
    void off(uint32_t generation, Ms now);
    bool timed_on(uint32_t seconds, Ms now);
    void observe(const Observation&, Ms now, int64_t utc_s, bool clock_valid);
    Decision tick(Ms now);
    void fault(Ms now);
    void clock_discontinuity(Ms now);
    uint32_t generation() const { return generation_; }
private:
    void invalidate();
    void expire(Ms now);
    Config config_{};
    uint32_t generation_ = 0;
    uint64_t last_request_ = 0;
    int64_t last_seen_source_ = 0;
    Ms lease_ = 0, ceiling_ = 0, override_ = 0, off_since_ = 0;
    bool home_ = false, commanded_ = false, fault_ = false, configured_ = false;
    double distance_ = -1;
};
// UTC becomes usable only after an SNTP sync; subsequent discontinuities invalidate
// evidence. No wall-clock value is ever used as a running lease deadline.
class ClockGuard {
public:
    bool update(Ms mono, int64_t utc, bool synced);
    bool jumped() const { return jumped_; }
private:
    Ms last_mono_ = 0;
    int64_t last_utc_ = 0;
    bool initialized_ = false, jumped_ = false;
};
}
