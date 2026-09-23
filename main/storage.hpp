#pragma once
#include "runtime.hpp"
#include "nvs.h"
namespace app {
class NvsStore : public Store {
public:
    bool initialize();
    ReadResult read(const char*,void*,size_t) override;
    bool write(const char*,const void*,size_t) override;
private:
    nvs_handle_t handle_=0;
    SemaphoreHandle_t mutex_=nullptr;
    StaticSemaphore_t mutex_data_{};
};
NvsStore& storage();
ReadResult load_profile(Profile&);
bool save_profile(Profile&);
bool valid_profile(const Profile&);
bool verify_password(const Profile&,const char* password);
bool password_hash(const char*,const uint8_t* salt,uint8_t* output);
enum class Change { Settings, Disabled, Auto, Arm, EnableOutput };
bool change_config(Change,uint32_t epoch,const Config* settings=nullptr);
}
