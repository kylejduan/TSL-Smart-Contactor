#include "test.hpp"
#include <iostream>
std::vector<Test>& tests() {static std::vector<Test> list;return list;}
int main() {
    unsigned failed=0;
    for(const auto& t:tests()) {
        try {t.run();std::cout<<"PASS "<<t.name<<'\n';}
        catch(const std::exception& e) {++failed;std::cerr<<"FAIL "<<t.name<<": "<<e.what()<<'\n';}
    }
    std::cout<<tests().size()<<" tests, "<<failed<<" failures\n";return failed?1:0;
}
