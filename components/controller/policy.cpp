#include "policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace tsl {
PositionBasis effective_position_basis(const Config& c) {
    return c.version==1 ? PositionBasis::GpsSource : static_cast<PositionBasis>(c.position_basis);
}
const char* position_basis_name(PositionBasis basis) {
    switch(basis) {
    case PositionBasis::GpsSource:return "gps_source";
    case PositionBasis::VehicleReport:return "vehicle_report";
    }
    return "unknown";
}
OutagePolicy effective_outage_policy(const Config& c) {
    return c.version<3 ? OutagePolicy::Expire : static_cast<OutagePolicy>(c.outage_policy);
}
const char* outage_policy_name(OutagePolicy policy) {
    switch(policy) {
    case OutagePolicy::Expire:return "expire";
    case OutagePolicy::HoldLast:return "hold_last";
    }
    return "unknown";
}
bool valid_vin(const char* s) {
    if (std::strlen(s) != 17) return false;
    for (int i = 0; i < 17; ++i)
        if (!((s[i] >= 'A' && s[i] <= 'Z' && s[i] != 'I' && s[i] != 'O' && s[i] != 'Q') ||
              (s[i] >= '0' && s[i] <= '9'))) return false;
    return true;
}
bool valid_config(const Config& c) {
    return (c.version == 1 || (c.version == 2 && c.position_basis<=1) ||
            (c.version == 3 && c.position_basis<=1 && c.outage_policy<=1)) &&
        c.vin[17] == 0 && valid_vin(c.vin) &&
        std::isfinite(c.home_lat) && std::isfinite(c.home_lon) &&
        std::abs(c.home_lat) <= 90 && std::abs(c.home_lon) <= 180 &&
        c.enable_m >= 10 && c.enable_m < c.disable_m && c.disable_m <= 10000 &&
        c.max_age_s >= 1 && c.max_age_s <= 600 && c.future_s <= 30 &&
        c.lease_s >= 60 && c.lease_s <= 900 && c.sleep_s >= c.lease_s && c.sleep_s <= 86400 &&
        c.poll_s >= 60 && c.poll_s <= 3600 && c.dwell_s >= 30 && c.dwell_s <= 600 &&
        c.daily_cap >= 1 && c.daily_cap <= 10000 && c.monthly_cap >= 1 &&
        c.monthly_cap <= 310000 && c.region <= 1;
}
double distance_m(double la, double lo, double lb, double lob) {
    constexpr double rad = 0.017453292519943295;
    const double a = std::sin((lb-la)*rad/2), b = std::sin((lob-lo)*rad/2);
    const double h = std::clamp(a*a + std::cos(la*rad)*std::cos(lb*rad)*b*b, 0.0, 1.0);
    return 6371008.8 * 2 * std::atan2(std::sqrt(h), std::sqrt(1-h));
}
void Policy::invalidate() {
    home_ = false; lease_ = 0; ceiling_ = 0;
    auto_state_ = AutoState::Unknown; restored_ = false;
}
void Policy::restore(AutoState state,int64_t source) {
    if(!configured_ || fault_ || !config_.commissioned || config_.disabled ||
       effective_outage_policy(config_)!=OutagePolicy::HoldLast ||
       (state!=AutoState::Home && state!=AutoState::Away))return;
    auto_state_=state;home_=state==AutoState::Home;restored_=true;last_seen_source_=source;
}
void Policy::expire(Ms now) {
    if (home_ && now >= lease_ && effective_outage_policy(config_)==OutagePolicy::Expire) invalidate();
    if (override_ && now >= override_) override_ = 0;
}
void Policy::configure(const Config& c, uint32_t gen, Ms now) {
    config_ = c; generation_ = gen; configured_ = valid_config(c);
    invalidate(); override_ = 0; last_seen_source_ = 0;
    last_request_ = 0; distance_ = -1;
    if (commanded_) off_since_ = now;
    commanded_ = false;
    if (!configured_) fault_ = true;
}
void Policy::off(uint32_t gen, Ms now) {
    generation_ = gen; config_.disabled = true; invalidate(); override_ = 0;
    if (commanded_) off_since_ = now;
    commanded_ = false;
}
void Policy::fault(Ms now) { fault_ = true; invalidate(); override_ = 0; tick(now); }
void Policy::clock_discontinuity(Ms now) {
    // A clock outage cannot confirm departure. Never claim retained evidence is
    // still fresh; admission of new evidence remains guarded by synchronized UTC.
    if (effective_outage_policy(config_)==OutagePolicy::HoldLast) { lease_=0;ceiling_=0; }
    else invalidate();
    (void)now; // The owning control tick applies the durable-commit gate.
}
bool Policy::timed_on(uint32_t seconds, Ms now) {
    expire(now);
    if (!configured_ || fault_ || !config_.commissioned || config_.disabled ||
        seconds == 0 || seconds > 28800) return false;
    override_ = now + Ms(seconds)*1000;
    return true;
}
void Policy::observe(const Observation& o, Ms now, int64_t utc, bool clock_valid) {
    expire(now);
    if (!configured_ || fault_ || config_.disabled || o.generation != generation_ ||
        o.request == 0 || o.request <= last_request_ ||
        o.vin[17] != 0 || std::strcmp(o.vin, config_.vin)) return;
    last_request_ = o.request;
    if (o.kind == Evidence::Revoked) { invalidate(); return; }
    if (!clock_valid) return;
    if (o.kind == Evidence::Asleep) {
        if (home_ && now < lease_ && now < ceiling_)
            lease_ = std::min(now + Ms(config_.lease_s)*1000, ceiling_);
        return;
    }
    if (o.kind != Evidence::Location || o.position_basis!=effective_position_basis(config_) ||
        (o.position_basis==PositionBasis::VehicleReport && o.vehicle!=Vehicle::Online) ||
        !o.quality_ok || !std::isfinite(o.lat) ||
        !std::isfinite(o.lon) || std::abs(o.lat) > 90 || std::abs(o.lon) > 180 ||
        o.source_s < 1577836800LL || o.source_s > 4102444800LL ||
        o.source_s <= last_seen_source_ || o.source_s > utc + config_.future_s ||
        utc-o.source_s > config_.max_age_s) return;
    last_seen_source_ = o.source_s;
    restored_ = false;
    distance_ = distance_m(config_.home_lat, config_.home_lon, o.lat, o.lon);
    if (distance_ >= config_.disable_m) { invalidate(); auto_state_=AutoState::Away; return; }
    if (distance_ <= config_.enable_m) { home_ = true; auto_state_=AutoState::Home; }
    if (!home_) return;
    // Anchor both deadlines to the explicitly selected timestamp basis. Report
    // age is a weaker opt-in proxy and never establishes GPS acquisition age.
    // A tolerated future timestamp grants no extra lease time.
    const Ms age = std::max<int64_t>(0, utc-o.source_s)*1000;
    lease_ = now + Ms(config_.lease_s)*1000 - age;
    ceiling_ = now + Ms(config_.sleep_s)*1000 - age;
}
Decision Policy::tick(Ms now, bool auto_commit_ready, bool output_inhibited) {
    expire(now);
    Decision d;
    d.auto_home = home_ && (now < lease_ || effective_outage_policy(config_)==OutagePolicy::HoldLast);
    d.retained = d.auto_home && now >= lease_;
    d.restored = restored_; d.auto_state = auto_state_;
    d.timed = override_ > now;
    d.lease_left = d.auto_home && now<lease_ ? lease_-now : 0;
    d.override_left = d.timed ? override_-now : 0;
    d.distance = distance_; d.last_source_s = last_seen_source_;
    if (fault_ || !configured_) d.reason = Reason::Fault;
    else if (!config_.commissioned) d.reason = Reason::Uncommissioned;
    else if (config_.disabled) d.reason = Reason::Disabled;
    else if (!(d.auto_home || d.timed)) d.reason = Reason::NoAuthorization;
    else {
        d.desired = true;
        d.reason = d.timed ? Reason::Timed : d.retained ? Reason::HomeRetained : Reason::Home;
        if (output_inhibited) d.reason = Reason::FirmwareUpdate;
        else if (!d.timed && !auto_commit_ready) d.reason = Reason::HomePending;
        else if (!commanded_ && now-off_since_ < Ms(config_.dwell_s)*1000) d.reason = Reason::OffDwell;
        else if (config_.dry_run) d.reason = Reason::DryRun;
        else d.commanded = true;
    }
    if (commanded_ && !d.commanded) off_since_ = now;
    commanded_ = d.commanded;
    return d;
}
bool recent_utc_sync(uint32_t now_uptime_s,uint32_t last_sync_uptime_s,bool synchronized) {
    // Unsigned elapsed time remains valid across the uptime-seconds wrap.
    return synchronized && uint32_t(now_uptime_s-last_sync_uptime_s)<=kUtcSyncMaxAgeS;
}
bool ClockGuard::update(Ms mono, int64_t utc, bool synced) {
    jumped_ = false;
    if (!synced || utc < 1704067200LL || utc > 4102444800LL) {
        jumped_ = initialized_; initialized_ = false; return false;
    }
    if (initialized_ && std::llabs((utc-last_utc_)*1000-(mono-last_mono_)) > 30000)
        jumped_ = true;
    last_mono_ = mono; last_utc_ = utc; initialized_ = true;
    return !jumped_;
}
const char* reason_name(Reason r) {
    switch (r) {
    case Reason::Uncommissioned: return "uncommissioned";
    case Reason::Disabled: return "user_disabled";
    case Reason::Fault: return "critical_local_fault";
    case Reason::NoAuthorization: return "no_auto_authorization";
    case Reason::Home: return "auto_home_lease";
    case Reason::HomeRetained: return "auto_home_retained";
    case Reason::HomePending: return "auto_state_commit_pending";
    case Reason::Timed: return "timed_override_bypasses_presence";
    case Reason::OffDwell: return "minimum_off_dwell";
    case Reason::DryRun: return "dry_run_output_inhibited";
    case Reason::ClockInvalid: return "utc_not_ready";
    case Reason::FirmwareUpdate: return "firmware_update_output_inhibited";
    }
    return "unknown";
}
const char* vehicle_name(Vehicle v) {
    switch (v) {
    case Vehicle::Online: return "online";
    case Vehicle::Asleep: return "asleep";
    case Vehicle::Offline: return "offline";
    default: return "unknown";
    }
}
}
