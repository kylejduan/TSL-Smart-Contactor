#pragma once
#include "policy.hpp"
#include "json.hpp"
#include <cstddef>
#include <string_view>
namespace tsl {
enum class Error : uint8_t {
    None, Transport, Timeout, TooLarge, Malformed, Authentication, Permission,
    RateLimit, Billing, Unavailable, Server, Redirect, Budget, Storage, Reauthorize, Clock, Wifi
};
const char* error_name(Error e); // Only these fixed labels go to logs/status.
Error http_error(int status);
// Optional detail is always a fixed literal, never upstream text or field values.
Error parse_vehicle(std::string_view body, const char* vin, bool location, Observation& o,
                    const char** detail=nullptr,double* gps_source_value=nullptr);
struct Tokens {
    char access[4097] = {}, refresh[2049] = {};
    uint32_t expires_s = 0;
};
Error parse_tokens(std::string_view body, Tokens& out);
bool parse_config(const Json&, int object, Config& out);
size_t url_encode(std::string_view value, char* out, size_t capacity);
bool constant_equal(std::string_view a, std::string_view b);
uint32_t crc32(const void* data, size_t size);
class BodyBuffer {
public:
    bool append(const char* data, size_t n);
    std::string_view view() const { return {bytes_, size_}; }
    bool overflow() const { return overflow_; }
private:
    char bytes_[16384] = {};
    size_t size_ = 0;
    bool overflow_ = false;
};
}
