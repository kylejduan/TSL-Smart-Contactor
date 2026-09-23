#include "json.hpp"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
namespace tsl {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
int hex(char c) {
    if (digit(c)) return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
}
void Json::ws() {
    while (pos_ < input_.size() && (input_[pos_]==' ' || input_[pos_]=='\n' ||
           input_[pos_]=='\r' || input_[pos_]=='\t')) ++pos_;
}
bool Json::quoted(Token& t) {
    t.start = ++pos_;
    while (pos_ < input_.size()) {
        unsigned char c = input_[pos_++];
        if (c == '"') { t.len = pos_-t.start-1; return true; }
        if (c < 32) return false;
        if (c == '\\') {
            if (pos_ == input_.size()) return false;
            char e = input_[pos_++];
            if (e == 'u') {
                for (int j=0; j<4; ++j)
                    if (pos_ == input_.size() || hex(input_[pos_++]) < 0) return false;
            } else if (!std::strchr("\"\\/bfnrt", e)) return false;
        }
    }
    return false;
}
int Json::value(unsigned depth) {
    ws();
    if (depth > 12 || count_ == tokens_.size() || pos_ == input_.size()) return -1;
    const int idx = count_++;
    Token& t = tokens_[idx];
    t.start = pos_;
    char c = input_[pos_];
    if (c == '{' || c == '[') {
        t.type = c == '{' ? Type::Object : Type::Array;
        char close = c == '{' ? '}' : ']'; ++pos_; ws();
        if (pos_ < input_.size() && input_[pos_] == close) ++pos_;
        else {
            while (true) {
                if (c == '{') {
                    int k = value(depth+1);
                    if (k < 0 || tokens_[k].type != Type::String) return -1;
                    auto key = input_.substr(tokens_[k].start, tokens_[k].len);
                    // Our keys are ASCII identifiers. Escaped keys are rejected to
                    // prevent aliases/duplicates such as vin and v\u0069n.
                    if (key.find('\\') != std::string_view::npos) return -1;
                    for (int prev = idx+1; prev < k; prev = tokens_[prev+1].end)
                        if (input_.substr(tokens_[prev].start, tokens_[prev].len) == key) return -1;
                    ws(); if (pos_ == input_.size() || input_[pos_++] != ':') return -1;
                }
                if (value(depth+1) < 0) return -1;
                ws(); if (pos_ == input_.size()) return -1;
                char sep = input_[pos_++];
                if (sep == close) break;
                if (sep != ',') return -1;
            }
        }
    } else if (c == '"') {
        t.type = Type::String;
        if (!quoted(t)) return -1;
    } else if (c == 't' || c == 'f' || c == 'n') {
        std::string_view lit = c=='t' ? "true" : (c=='f' ? "false" : "null");
        if (input_.substr(pos_, lit.size()) != lit) return -1;
        pos_ += lit.size(); t.type = c=='n' ? Type::Null : Type::Bool;
    } else {
        t.type = Type::Number;
        if (c == '-') ++pos_;
        if (pos_ == input_.size() || !digit(input_[pos_])) return -1;
        if (input_[pos_] == '0') ++pos_;
        else while (pos_ < input_.size() && digit(input_[pos_])) ++pos_;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_; size_t begin = pos_;
            while (pos_ < input_.size() && digit(input_[pos_])) ++pos_;
            if (begin == pos_) return -1;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_]=='+' || input_[pos_]=='-')) ++pos_;
            size_t begin = pos_;
            while (pos_ < input_.size() && digit(input_[pos_])) ++pos_;
            if (begin == pos_) return -1;
        }
    }
    if (t.type != Type::String) t.len = pos_-t.start;
    t.end = count_;
    return idx;
}
bool Json::parse(std::string_view s) {
    input_ = s; pos_ = count_ = 0;
    if (s.empty() || s.size() > 16384) return false;
    if (value(0) != 0) { count_ = 0; return false; }
    ws(); if (pos_ != s.size()) { count_ = 0; return false; }
    return true;
}
bool Json::is(int i, Type t) const { return i >= 0 && size_t(i) < count_ && tokens_[i].type == t; }
int Json::get(int o, std::string_view k) const {
    if (!is(o, Type::Object)) return -1;
    for (int i=o+1; i<tokens_[o].end; i=tokens_[i+1].end)
        if (input_.substr(tokens_[i].start, tokens_[i].len) == k) return i+1;
    return -1;
}
int Json::at(int a, size_t n) const {
    if (!is(a, Type::Array)) return -1;
    for (int i=a+1; i<tokens_[a].end; i=tokens_[i].end) if (n-- == 0) return i;
    return -1;
}
bool Json::string(int i, char* dst, size_t cap) const {
    if (!is(i, Type::String) || cap == 0) return false;
    auto s = input_.substr(tokens_[i].start, tokens_[i].len); size_t out = 0;
    auto append = [&](unsigned char c) { if (!c || out+1 >= cap) return false; dst[out++] = c; return true; };
    for (size_t p=0; p<s.size(); ++p) {
        unsigned char c = s[p];
        if (c != '\\') { if (!append(c)) return false; continue; }
        c = s[++p];
        if (c == 'u') {
            uint32_t cp = 0;
            for (int k=0;k<4;++k) cp=cp*16+hex(s[++p]);
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (p+6 >= s.size() || s[p+1]!='\\' || s[p+2]!='u') return false;
                p+=2; uint32_t low=0;
                for (int k=0;k<4;++k) low=low*16+hex(s[++p]);
                if (low<0xdc00 || low>0xdfff) return false;
                cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
            } else if (cp>=0xdc00 && cp<=0xdfff) return false;
            if (cp<0x80) { if (!append(cp)) return false; }
            else if (cp<0x800) { if (!append(0xc0|(cp>>6)) || !append(0x80|(cp&63))) return false; }
            else if (cp<0x10000) {
                if (!append(0xe0|(cp>>12)) || !append(0x80|((cp>>6)&63)) || !append(0x80|(cp&63))) return false;
            } else if (!append(0xf0|(cp>>18)) || !append(0x80|((cp>>12)&63)) ||
                       !append(0x80|((cp>>6)&63)) || !append(0x80|(cp&63))) return false;
        } else {
            switch(c) { case 'b': c='\b'; break; case 'f': c='\f'; break;
                case 'n': c='\n'; break; case 'r': c='\r'; break; case 't': c='\t'; break; default: break; }
            if (!append(c)) return false;
        }
    }
    dst[out] = 0; return true;
}
bool Json::equal(int i, std::string_view s) const {
    return is(i,Type::String) && input_.substr(tokens_[i].start,tokens_[i].len)==s;
}
bool Json::number(int i, double& v) const {
    char b[64] = {};
    if (!number_text(i,b,sizeof b)) return false;
    errno=0; char* end=nullptr; v=std::strtod(b,&end);
    return !errno && end && !*end && std::isfinite(v);
}
bool Json::number_text(int i, char* dst, size_t capacity) const {
    if (!dst || !capacity) return false;
    dst[0]=0;
    if (!is(i,Type::Number) || tokens_[i].len>=capacity) return false;
    // Only the JSON number grammar is exposed, never strings or arbitrary body text.
    std::memcpy(dst,input_.data()+tokens_[i].start,tokens_[i].len);
    dst[tokens_[i].len]=0;
    return true;
}
bool Json::integer(int i, int64_t& v) const {
    if (!is(i, Type::Number)) return false;
    auto s=input_.substr(tokens_[i].start,tokens_[i].len);
    if (s.empty() || s.size()>19 || s[0]=='-') return false;
    uint64_t n=0;
    for (char c:s) {
        if (!digit(c) || n>(uint64_t(INT64_MAX)-uint64_t(c-'0'))/10) return false;
        n=n*10+(c-'0');
    }
    v=static_cast<int64_t>(n); return true;
}
bool Json::boolean(int i, bool& v) const {
    if (!is(i,Type::Bool)) return false;
    v=input_[tokens_[i].start]=='t'; return true;
}
}
