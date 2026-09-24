#pragma once
#include "protocol.hpp"
namespace tsl {
// Strict bounded HTTP/1 response decoder used by the ESP TLS transport and tests.
// Handles arbitrarily split headers/chunks without allocating or decompressing.
class HttpDecoder {
public:
    explicit HttpDecoder(BodyBuffer& body) : body_(body) {}
    bool feed(const char*,size_t);
    bool eof();
    bool done() const {return state_==State::Done;}
    Error error() const {return error_;}
    int status() const {return status_;}
    const char* retry_after() const {return retry_;}
    const char* transaction_id() const {return txid_;}
    const char* response_date() const {return date_;}
    std::string_view body() const {return body_.view();}
private:
    enum class State {Status,Headers,Fixed,Close,ChunkSize,ChunkData,ChunkCr,ChunkLf,Trailers,Done};
    State state_=State::Status;
    Error error_=Error::None;
    BodyBuffer& body_;
    char line_[1024]={},retry_[128]={};
    char txid_[129]={},date_[30]={};
    bool saw_txid_=false,saw_date_=false;
    size_t line_size_=0,header_bytes_=0,remaining_=0;
    bool saw_cr_=false,has_length_=false,chunked_=false;
    int status_=0;
    bool line();
    bool fail(Error e=Error::Malformed) {error_=e;return false;}
};
}
