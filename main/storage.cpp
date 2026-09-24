#include "storage.hpp"
#include "nvs_flash.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include <cstring>
namespace app {
static StaticSemaphore_t config_mutex_data;
static SemaphoreHandle_t config_mutex=nullptr;
NvsStore& storage() { static NvsStore instance;return instance; }
bool NvsStore::initialize() {
    mutex_=xSemaphoreCreateMutexStatic(&mutex_data_);
    config_mutex=xSemaphoreCreateMutexStatic(&config_mutex_data);
    // Never erase NVS on version, space, corruption or initialization errors.
    return mutex_ && nvs_flash_init()==ESP_OK && nvs_open("controller",NVS_READWRITE,&handle_)==ESP_OK;
}
ReadResult NvsStore::read(const char* key,void* p,size_t n) {
    if(!handle_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(5000))!=pdTRUE)return ReadResult::Failed;
    size_t len=n;esp_err_t e=nvs_get_blob(handle_,key,p,&len);
    xSemaphoreGive(mutex_);
    if(e==ESP_ERR_NVS_NOT_FOUND)return ReadResult::Missing;
    return e==ESP_OK && n==len ? ReadResult::Ok : ReadResult::Failed;
}
bool NvsStore::write(const char* key,const void* p,size_t n) {
    if(!handle_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(5000))!=pdTRUE)return false;
    bool ok=nvs_set_blob(handle_,key,p,n)==ESP_OK && nvs_commit(handle_)==ESP_OK;
    xSemaphoreGive(mutex_);return ok;
}
ReadResult load_profile(Profile& p) {
    uint8_t pending=0;auto state=storage().read("provisioning",&pending,sizeof pending);
    if(state==ReadResult::Failed || (state==ReadResult::Ok && pending))return ReadResult::Failed;
    auto r=storage().read("profile",&p,sizeof p);
    return r==ReadResult::Ok && (!intact_profile(p) || !valid_config(p.config)) ? ReadResult::Failed : r;
}
bool intact_profile(const Profile& p) {
    return (p.version==1 || p.version==2) && p.crc==crc32(&p,offsetof(Profile,crc));
}
bool save_profile(Profile& p) {
    seal(p);return storage().write("profile",&p,sizeof p);
}
bool password_hash(const char* pw,const uint8_t* salt,uint8_t* out) {
    // 100k PBKDF2-SHA256. Browser login computes this on the client for v2.
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        reinterpret_cast<const unsigned char*>(pw),std::strlen(pw),salt,16,100000,32,out)==0;
}
bool password_verifier(const uint8_t* material,uint8_t* out) {
    // Domain separation prevents the stored digest from being a usable login key.
    uint8_t input[80]="TSL-Smart-Contactor password verifier v2";
    constexpr size_t label_size=sizeof("TSL-Smart-Contactor password verifier v2")-1;
    static_assert(label_size+32<=sizeof input);
    std::memcpy(input+label_size,material,32);
    bool ok=mbedtls_sha256(input,label_size+32,out,0)==0;
    mbedtls_platform_zeroize(input,sizeof input);return ok;
}
bool verify_password(const Profile& p,const char* pw) {
    if(std::strlen(pw)<16 || std::strlen(pw)>128)return false;
    uint8_t material[32]={},verifier[32]={};bool ok=password_hash(pw,p.salt,material);
    if(ok && p.version==2)ok=password_verifier(material,verifier);
    if(ok) {
        const auto* candidate=p.version==2 ? verifier : material;
        ok=constant_equal({reinterpret_cast<const char*>(candidate),32},
                          {reinterpret_cast<const char*>(p.password_hash),32});
    }
    mbedtls_platform_zeroize(material,sizeof material);
    mbedtls_platform_zeroize(verifier,sizeof verifier);return ok;
}
bool verify_derived(const Profile& p,const char* hex) {
    if(p.version!=2 || std::strlen(hex)!=64)return false;
    uint8_t material[32]={},verifier[32]={};
    auto nibble=[](char c)->int {return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1;};
    bool ok=true;
    for(size_t i=0;i<32;++i) {
        int a=nibble(hex[i*2]),b=nibble(hex[i*2+1]);
        if(a<0 || b<0) {ok=false;break;}
        material[i]=uint8_t((a<<4)|b);
    }
    ok=ok && password_verifier(material,verifier);
    ok=ok && constant_equal({reinterpret_cast<const char*>(verifier),32},
                            {reinterpret_cast<const char*>(p.password_hash),32});
    mbedtls_platform_zeroize(material,sizeof material);
    mbedtls_platform_zeroize(verifier,sizeof verifier);return ok;
}
bool migrate_password(Profile& running) {
    if(running.version!=1 || !config_mutex ||
       xSemaphoreTake(config_mutex,pdMS_TO_TICKS(5000))!=pdTRUE)return false;
    static Profile next;
    bool ok=load_profile(next)==ReadResult::Ok && next.version==1 &&
        constant_equal({reinterpret_cast<const char*>(next.salt),16},
                       {reinterpret_cast<const char*>(running.salt),16}) &&
        constant_equal({reinterpret_cast<const char*>(next.password_hash),32},
                       {reinterpret_cast<const char*>(running.password_hash),32});
    if(ok) {
        uint8_t verifier[32]={};
        ok=password_verifier(next.password_hash,verifier);
        if(ok) {
            next.version=2;std::memcpy(next.password_hash,verifier,32);
            ok=save_profile(next);
            if(ok) {
                std::memcpy(running.password_hash,verifier,32);
                running.version=2;
            }
        }
        mbedtls_platform_zeroize(verifier,sizeof verifier);
    }
    mbedtls_platform_zeroize(&next,sizeof next);
    xSemaphoreGive(config_mutex);return ok;
}
static int rng(void*,unsigned char* b,size_t n) {esp_fill_random(b,n);return 0;}
bool valid_profile(const Profile& p) {
    if(!valid_config(p.config) || p.ssid[32] || p.wifi_password[64] || p.client_id[128] ||
       p.certificate[2048] || p.private_key[4096] || p.origin[192] || !p.ssid[0] || !p.client_id[0])return false;
    size_t wifi_len=std::strlen(p.wifi_password);
    if(wifi_len<8 || wifi_len>63)return false;
    std::string_view origin(p.origin);
    if(origin.substr(0,8)!="https://" || origin.size()<9 || origin.find_first_of("\r\n\t ?#@\\",8)!=origin.npos ||
       origin.find('/',8)!=origin.npos)return false;
    mbedtls_x509_crt cert;mbedtls_x509_crt_init(&cert);
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    bool ok=mbedtls_x509_crt_parse(&cert,reinterpret_cast<const uint8_t*>(p.certificate),std::strlen(p.certificate)+1)==0;
    ok=ok && mbedtls_pk_parse_key(&key,reinterpret_cast<const uint8_t*>(p.private_key),std::strlen(p.private_key)+1,nullptr,0,rng,nullptr)==0;
    ok=ok && mbedtls_pk_check_pair(&cert.pk,&key,rng,nullptr)==0;
    mbedtls_pk_free(&key);mbedtls_x509_crt_free(&cert);return ok;
}
bool change_config(Change change,uint32_t epoch,const Config* settings) {
    if(!config_mutex || xSemaphoreTake(config_mutex,pdMS_TO_TICKS(5000))!=pdTRUE)return false;
    if(snapshot().generation!=epoch) {xSemaphoreGive(config_mutex);return false;}
    Profile p;
    bool ok=load_profile(p)==ReadResult::Ok;
    if(ok) {
        switch(change) {
        case Change::Settings: if(settings)p.config=*settings;else ok=false;break;
        case Change::Disabled:p.config.disabled=true;break;
        case Change::Auto:p.config.disabled=false;break;
        case Change::Arm:p.config.commissioned=true;p.config.disabled=true;break;
        case Change::EnableOutput:
            ok=p.config.commissioned;p.config.dry_run=false;p.config.disabled=true;break;
        }
    }
    ok=ok && valid_config(p.config) && save_profile(p);
    bool superseded=snapshot().generation!=epoch;
    if(ok && superseded) {
        // OFF can preempt an NVS write. Before releasing this writer, overwrite
        // any just-saved AUTO with DISABLED. The newer request then applies itself.
        p.config.disabled=true;ok=save_profile(p);
    }
    if(!ok)fail("configuration_write");
    Config c=p.config;mbedtls_platform_zeroize(&p,sizeof p);
    if(ok && !superseded)ok=configure(c,epoch);
    xSemaphoreGive(config_mutex);return ok && !superseded;
}
}
