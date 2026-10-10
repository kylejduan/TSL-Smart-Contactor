#pragma once
#include "reliability.hpp"
namespace tsl {
// A decision, not GPIO state, source data, a lease or a manual override.
struct AutoRecord {
    uint32_t version=1, config_crc=0;
    AutoState state=AutoState::Unknown;
    uint8_t reserved[7]{};
    int64_t order_source_s=0; // Ordering watermark, never a reusable freshness lease.
    uint32_t reserved_word=0;
    uint32_t crc=0;
};
static_assert(sizeof(AutoRecord)==32 && offsetof(AutoRecord,crc)==28);
uint32_t auto_config_crc(const Config&);
// Boot recovery also checks durable token revocation, without any API call.
Error recover_auto_state(Store&,const Config&,bool allow_restore,AutoState&,int64_t&);
class AutoJournal {
public:
    explicit AutoJournal(Store& store):store_(store) {}
    Error load(const Config&,AutoState&,int64_t* order_source_s=nullptr);
    Error save(const Config&,AutoState,int64_t order_source_s=0);
    Error clear();
private:
    Error read(AutoRecord&,bool& present);
    Error replace(const AutoRecord&);
    Store& store_;
};
}
