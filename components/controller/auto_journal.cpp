#include "auto_journal.hpp"
#include "client.hpp"
namespace tsl {
uint32_t auto_config_crc(const Config& c) {
    // Include every setting without depending on compiler padding bytes.
    uint8_t bytes[sizeof(Config)]{};size_t used=0;
    auto append=[&](const auto& value) {
        std::memcpy(bytes+used,&value,sizeof value);used+=sizeof value;
    };
    append(c.version);append(c.vin);append(c.home_lat);append(c.home_lon);
    append(c.enable_m);append(c.disable_m);append(c.max_age_s);append(c.future_s);
    append(c.lease_s);append(c.sleep_s);append(c.poll_s);append(c.dwell_s);
    append(c.daily_cap);append(c.monthly_cap);append(c.commissioned);append(c.disabled);
    append(c.dry_run);append(c.region);append(c.position_basis);append(c.outage_policy);
    return crc32(bytes,used);
}
Error AutoJournal::read(AutoRecord& record,bool& present) {
    auto result=store_.read("auto_state",&record,sizeof record);
    present=result==ReadResult::Ok;
    if(result==ReadResult::Missing)return Error::None;
    if(result!=ReadResult::Ok || !intact(record) || uint8_t(record.state)>2 || record.reserved_word ||
       (record.order_source_s && (record.order_source_s<1577836800LL ||
                                 record.order_source_s>4102444800LL)))return Error::Storage;
    for(auto byte:record.reserved)if(byte)return Error::Storage;
    return Error::None;
}
Error AutoJournal::load(const Config& config,AutoState& state,int64_t* order_source_s) {
    state=AutoState::Unknown;
    if(order_source_s)*order_source_s=0;
    AutoRecord record;bool present;
    auto error=read(record,present);
    if(error!=Error::None)return error;
    if(!valid_config(config))return Error::Storage;
    if(present && record.config_crc==auto_config_crc(config) &&
       config.commissioned && !config.disabled &&
       effective_outage_policy(config)==OutagePolicy::HoldLast) {
        state=record.state;if(order_source_s)*order_source_s=record.order_source_s;
    }
    return Error::None;
}
Error AutoJournal::replace(const AutoRecord& next) {
    AutoRecord old;bool present;
    auto error=read(old,present);
    if(error!=Error::None)return error; // Corruption requires explicit recovery.
    if(present && old.config_crc==next.config_crc && old.state==next.state)return Error::None;
    return store_.write("auto_state",&next,sizeof next) ? Error::None : Error::Storage;
}
Error AutoJournal::save(const Config& config,AutoState state,int64_t source) {
    if(!valid_config(config) || uint8_t(state)>2 ||
       (source && (source<1577836800LL || source>4102444800LL)))return Error::Storage;
    AutoRecord record;
    record.config_crc=auto_config_crc(config);record.state=state;
    record.order_source_s=state==AutoState::Unknown ? 0 : source;seal(record);
    return replace(record);
}
Error AutoJournal::clear() {
    AutoRecord record;seal(record);return replace(record);
}
Error recover_auto_state(Store& store,const Config& config,bool allow,AutoState& state,int64_t& source) {
    AutoJournal journal(store);auto result=journal.load(config,state,&source);
    if(result==Error::None && state==AutoState::Home) {
        TokenJournal tokens(store);auto status=tokens.load();wipe(&tokens,sizeof tokens);
        if(status==Error::Storage)result=Error::Storage;
        else if(status!=Error::None)allow=false;
    }
    if(result!=Error::None) {state=AutoState::Unknown;source=0;return result;}
    if(!allow) {state=AutoState::Unknown;source=0;return journal.clear();}
    return Error::None;
}
}
