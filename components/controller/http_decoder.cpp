#include "http_decoder.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
namespace tsl {
bool HttpDecoder::line() {
    line_[line_size_]=0;
    std::string_view s(line_,line_size_);
    if(state_==State::Status) {
        if(s.size()<12 || (s.substr(0,9)!="HTTP/1.1 " && s.substr(0,9)!="HTTP/1.0 ") ||
           !std::isdigit(static_cast<unsigned char>(s[9])) || !std::isdigit(static_cast<unsigned char>(s[10])) || !std::isdigit(static_cast<unsigned char>(s[11])) ||
           (s.size()>12 && s[12]!=' '))return fail();
        status_=(s[9]-'0')*100+(s[10]-'0')*10+s[11]-'0';
        if(status_<200 || status_>599)return fail();
        state_=State::Headers;return true;
    }
    if(state_==State::ChunkSize) {
        auto end=s.find(';');s=s.substr(0,end);
        if(s.empty() || s.size()>8)return fail();
        remaining_=0;
        for(char c:s) {
            unsigned v;
            if(c>='0' && c<='9')v=c-'0';
            else if(c>='a' && c<='f')v=c-'a'+10;
            else if(c>='A' && c<='F')v=c-'A'+10;
            else return fail();
            remaining_=remaining_*16+v;
            if(remaining_>16384)return fail(Error::TooLarge);
        }
        state_=remaining_ ? State::ChunkData : State::Trailers;return true;
    }
    if(s.empty()) {
        if(state_==State::Trailers) {state_=State::Done;return true;}
        if(chunked_ && has_length_)return fail();
        if(chunked_)state_=State::ChunkSize;
        else if(has_length_)state_=remaining_ ? State::Fixed : State::Done;
        else state_=State::Close;
        return true;
    }
    auto colon=s.find(':');
    if(colon==s.npos || colon==0)return fail();
    for(size_t i=0;i<colon;++i) {
        unsigned char c=line_[i];
        if(!std::isalnum(c) && c!='-')return fail();
        line_[i]=std::tolower(c);
    }
    auto name=s.substr(0,colon),v=s.substr(colon+1);
    while(!v.empty() && (v.front()==' ' || v.front()=='\t'))v.remove_prefix(1);
    while(!v.empty() && (v.back()==' ' || v.back()=='\t'))v.remove_suffix(1);
    if(state_==State::Trailers)return true;
    if(name=="content-length") {
        if(has_length_ || v.empty() || v.size()>10)return fail();
        has_length_=true;remaining_=0;
        for(char c:v) {
            if(c<'0' || c>'9')return fail();
            remaining_=remaining_*10+c-'0';
            if(remaining_>16384)return fail(Error::TooLarge);
        }
    } else if(name=="transfer-encoding") {
        if(chunked_ || v!="chunked")return fail();
        chunked_=true;
    } else if(name=="content-encoding" && v!="identity")return fail();
    else if(name=="retry-after") {
        if(v.size()>=sizeof retry_ || retry_[0])return fail();
        std::memcpy(retry_,v.data(),v.size());retry_[v.size()]=0;
    }
    return true;
}
bool HttpDecoder::feed(const char* data,size_t size) {
    if(error_!=Error::None)return false;
    for(size_t i=0;i<size;++i) {
        char c=data[i];
        if(state_==State::Done)return fail(); // no pipelining or hidden trailing data
        if(state_==State::Fixed || state_==State::Close || state_==State::ChunkData) {
            if(!body_.append(&c,1))return fail(Error::TooLarge);
            if(state_!=State::Close && --remaining_==0)
                state_=state_==State::Fixed ? State::Done : State::ChunkCr;
        } else if(state_==State::ChunkCr) {
            if(c!='\r')return fail();
            state_=State::ChunkLf;
        } else if(state_==State::ChunkLf) {
            if(c!='\n')return fail();
            state_=State::ChunkSize;
        } else {
            if(++header_bytes_>8192)return fail(Error::TooLarge);
            if(saw_cr_) {
                if(c!='\n')return fail();
                saw_cr_=false;
                if(!line())return false;
                line_size_=0;
            } else if(c=='\r')saw_cr_=true;
            else {
                if((static_cast<unsigned char>(c)<32 && c!='\t') || line_size_+1>=sizeof line_)return fail();
                line_[line_size_++]=c;
            }
        }
    }
    return true;
}
bool HttpDecoder::eof() {
    if(state_==State::Close)state_=State::Done;
    if(state_!=State::Done)return fail();
    return error_==Error::None;
}
}
