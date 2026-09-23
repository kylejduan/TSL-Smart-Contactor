#pragma once
#include "policy.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>
struct Test {const char* name;void (*run)();};
std::vector<Test>& tests();
struct Register {Register(const char* n,void (*fn)()) {tests().push_back({n,fn});}};
#define TEST(name) static void name(); static Register reg_##name(#name,name); static void name()
#define REQUIRE(expr) do {if(!(expr))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #expr);}while(false)
inline tsl::Config config() {
    tsl::Config c;std::strcpy(c.vin,"5YJ3E1EA7KF000001");
    c.home_lat=0;c.home_lon=0;c.commissioned=true;c.disabled=false;c.dry_run=false;return c;
}
inline tsl::Observation fix(int64_t source,uint64_t request=1,double lon=0,uint32_t gen=1) {
    tsl::Observation o;o.generation=gen;o.request=request;std::strcpy(o.vin,config().vin);
    o.kind=tsl::Evidence::Location;o.vehicle=tsl::Vehicle::Online;o.source_s=source;o.lat=0;o.lon=lon;return o;
}
inline constexpr int64_t epoch=1800000000;
