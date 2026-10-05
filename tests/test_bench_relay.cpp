#include "bench_protocol.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr std::string_view nonce="0123456789abcdef0123456789abcdef";
constexpr std::string_view other="fedcba9876543210fedcba9876543210";
#define CHECK(condition) do {if(!(condition))throw std::runtime_error( \
    std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #condition);}while(false)
bench::Reply command(bench::Session& s,std::string_view text,bench::Ms now) {
    for(char c:text)s.receive(c,now);
    return s.receive('\n',now);
}
std::string start(std::string_view challenge=nonce) {return "START "+std::string(challenge);}

void boot_and_dwell() {
    bench::Session s(10,nonce);
    CHECK(!s.on() && !s.used() && !s.fault());
    CHECK(s.dwell_remaining(10)==30000);
    CHECK(command(s,"STATUS",10)==bench::Reply::Status);
    CHECK(command(s,start(),30009)==bench::Reply::Dwell);
    CHECK(!s.on() && !s.used());
    s.tick(30010);
    CHECK(!s.on()); // An early request is rejected, never queued.
    CHECK(s.dwell_remaining(30010)==0);
    CHECK(command(s,start(),30010)==bench::Reply::Started);
    CHECK(s.on() && s.used());
}
void pulse_and_single_use() {
    bench::Session s(0,nonce);
    CHECK(command(s,start(),30000)==bench::Reply::Started);
    s.tick(31999);CHECK(s.on());
    CHECK(command(s,start(),31999)==bench::Reply::Used);
    s.tick(32000);CHECK(!s.on());
    CHECK(command(s,start(),50000)==bench::Reply::Used);
    CHECK(!s.on());
}
void immediate_off_and_no_rearm() {
    bench::Session s(0,nonce);
    CHECK(command(s,start(),30000)==bench::Reply::Started);
    CHECK(command(s,"OFF",30001)==bench::Reply::Off);
    CHECK(!s.on() && s.used());
    CHECK(command(s,start(),30002)==bench::Reply::Used);
    bench::Session fresh(0,nonce);
    CHECK(command(fresh,"OFF",0)==bench::Reply::Off);
    CHECK(command(fresh,start(),30000)==bench::Reply::Used);
    CHECK(!fresh.on());
}
void reset_and_old_nonce() {
    bench::Session before(0,nonce);
    CHECK(command(before,start(),30000)==bench::Reply::Started);
    bench::Session after(0,other);
    CHECK(!after.on() && !after.used());
    CHECK(command(after,start(),30000)==bench::Reply::WrongNonce);
    CHECK(!after.on() && !after.used());
    CHECK(command(after,start(other),30000)==bench::Reply::Started);
}
void exact_commands_and_binary_rejection() {
    for(const std::string& text: {
        std::string(""),std::string("start ")+std::string(nonce),
        std::string(" START ")+std::string(nonce),start()+" ",
        std::string("START "),std::string("ON"),std::string("AUTO"),
        std::string("START ")+std::string(32,'A'),
        std::string("START ")+std::string(nonce.substr(0,16))+"\r"+std::string(nonce.substr(16)),
        std::string("START ")+std::string(nonce)+std::string(1,'\0')}) {
        bench::Session s(0,nonce);
        CHECK(!bench::accepted(command(s,text,30000)));
        CHECK(!s.on() && !s.used());
    }
    bench::Session s(0,nonce);
    CHECK(command(s,start()+"\r",30000)==bench::Reply::Started); // CRLF framing.
}
void oversized_and_slow_frames() {
    bench::Session s(0,nonce);
    CHECK(command(s,std::string(10000,'x')+start(),30000)==bench::Reply::Malformed);
    CHECK(!s.on());
    for(char c:std::string("START "))s.receive(c,30000);
    s.tick(31000); // Fixed whole-frame deadline, not a sliding byte deadline.
    CHECK(command(s,nonce,31000)==bench::Reply::Malformed);
    CHECK(!s.on() && !s.used());
    CHECK(command(s,start(),31000)==bench::Reply::Started);
    for(char c:std::string(10000,'x'))s.receive(c,32000);
    s.tick(33000);CHECK(!s.on()); // Partial/bad input never postpones expiry.
    CHECK(s.receive('\n',33000)==bench::Reply::Malformed);
    CHECK(command(s,"OFF",33000)==bench::Reply::Off);
}
void faults_and_invalid_boot_challenges() {
    bench::Session s(0,nonce);
    CHECK(command(s,start(),30000)==bench::Reply::Started);
    s.fail();CHECK(s.fault() && !s.on() && s.used());
    CHECK(command(s,start(),30001)==bench::Reply::Fault);
    CHECK(command(s,"STATUS",30002)==bench::Reply::Status);
    CHECK(command(s,"OFF",30003)==bench::Reply::Off);
    CHECK(s.fault() && !s.on());
    for(std::string_view invalid:{std::string_view(""),nonce.substr(0,31),
        std::string_view("0123456789ABCDEF0123456789ABCDEF")}) {
        bench::Session bad(0,invalid);
        CHECK(bad.fault() && bad.used() && !bad.on());
    }
}
void monotonic_time_and_long_uptime() {
    constexpr bench::Ms boot=1ULL<<48;
    bench::Session s(boot,nonce);
    CHECK(command(s,start(),boot+30000)==bench::Reply::Started);
    s.tick(boot+32000);CHECK(!s.on());
    bench::Session backwards(0,nonce);
    CHECK(command(backwards,start(),30000)==bench::Reply::Started);
    backwards.tick(29999);
    CHECK(backwards.fault() && !backwards.on());
}
void off_wins_queued_commands() {
    bench::Session s(0,nonce);
    const std::string burst=start()+"\nSTATUS\nSTATUS\nOFF\n"+start()+"\n";
    bench::Reply result=bench::Reply::None;
    for(char c:burst) {
        const auto reply=s.receive(c,30000);
        if(reply!=bench::Reply::None)result=reply;
    }
    CHECK(result==bench::Reply::Used);
    CHECK(!s.on() && s.used());
}
}
int main() {
    const struct {const char* name;void (*run)();} cases[]={
        {"boot_and_dwell",boot_and_dwell},{"pulse_and_single_use",pulse_and_single_use},
        {"immediate_off_and_no_rearm",immediate_off_and_no_rearm},
        {"reset_and_old_nonce",reset_and_old_nonce},
        {"exact_commands_and_binary_rejection",exact_commands_and_binary_rejection},
        {"oversized_and_slow_frames",oversized_and_slow_frames},
        {"faults_and_invalid_boot_challenges",faults_and_invalid_boot_challenges},
        {"monotonic_time_and_long_uptime",monotonic_time_and_long_uptime},
        {"off_wins_queued_commands",off_wins_queued_commands}};
    int failed=0;
    for(const auto& test:cases) {
        try {test.run();std::cout<<"PASS "<<test.name<<'\n';}
        catch(const std::exception& e) {++failed;std::cerr<<"FAIL "<<test.name<<": "<<e.what()<<'\n';}
    }
    std::cout<<sizeof cases/sizeof cases[0]<<" bench tests, "<<failed<<" failures\n";
    return failed?1:0;
}
