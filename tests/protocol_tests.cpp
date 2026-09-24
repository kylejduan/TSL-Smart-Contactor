#include "test.hpp"
#include "protocol.hpp"
using namespace tsl;
static std::string body(const std::string& fields) {
    return "{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\"online\",\"drive_state\":{"+fields+"}}}";
}
TEST(real_parser_uses_drive_state_gps_source_only) {
    Observation o;
    REQUIRE(parse_vehicle(body("\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000,\"timestamp\":1800000600000"),config().vin,true,o)==Error::None);
    REQUIRE(o.source_s==epoch);REQUIRE(o.kind==Evidence::Location);
    REQUIRE(parse_vehicle(body("\"latitude\":0,\"longitude\":0,\"timestamp\":1800000000000"),config().vin,true,o)==Error::Malformed);
}
TEST(missing_null_invalid_and_wrong_units_fail_closed) {
    for(auto fields:{"\"latitude\":null,\"longitude\":0,\"gps_as_of\":1800000000",
        "\"latitude\":0,\"gps_as_of\":1800000000", "\"latitude\":91,\"longitude\":0,\"gps_as_of\":1800000000",
        "\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000000",
        "\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000.5",
        "\"latitude\":\"0\",\"longitude\":0,\"gps_as_of\":1800000000"}) {
        Observation o;REQUIRE(parse_vehicle(body(fields),config().vin,true,o)==Error::Malformed);
    }
}
TEST(status_does_not_confuse_online_offline_sleep_or_home) {
    for(auto state:{"asleep","online","offline","unknown"}) {
        Observation o;std::string b="{\"response\":{\"vin\":\"5YJ3E1EA7KF000001\",\"state\":\""+std::string(state)+"\"}}";
        REQUIRE(parse_vehicle(b,config().vin,false,o)==Error::None);
        REQUIRE((o.kind==Evidence::Asleep)==(std::string(state)=="asleep"));
    }
}
TEST(location_diagnostics_distinguish_missing_null_and_invalid_source_without_values) {
    const std::string coordinates="\"latitude\":12.3456,\"longitude\":65.4321";
    for(const auto& item:{std::pair{"","gps_as_of_missing"},
        {",\"gps_as_of\":null","gps_as_of_null"},
        {",\"gps_as_of\":1800000000000","gps_as_of_millisecond_scale"},
        {",\"gps_as_of\":0","gps_as_of_out_of_range"},
        {",\"gps_as_of\":\"1800000000\"","gps_as_of_not_numeric"},
        {",\"gps_as_of\":1800000000.5","gps_as_of_fractional_seconds"}}) {
        Observation o;const char* detail=nullptr;double source=999;
        REQUIRE(parse_vehicle(body(coordinates+item.first),config().vin,true,o,&detail,&source)==Error::Malformed);
        REQUIRE(std::string(detail)==item.second);REQUIRE(o.kind!=Evidence::Location);
        if(std::string(item.second)=="gps_as_of_millisecond_scale")REQUIRE(source==1800000000000.0);
        else if(std::string(item.second)=="gps_as_of_out_of_range")REQUIRE(source==0);
        else if(std::string(item.second)=="gps_as_of_fractional_seconds")REQUIRE(source==1800000000.5);
        else REQUIRE(source==-1);
    }
    Observation o;const char* detail=nullptr;
    REQUIRE(parse_vehicle("{\"response\":null,\"error\":\"private upstream text\"}",config().vin,true,o,&detail)==Error::Malformed);
    REQUIRE(std::string(detail)=="response_not_object");
    REQUIRE(parse_vehicle(body(coordinates+",\"gps_as_of\":1800000000"),config().vin,true,o,&detail)==Error::None);
    REQUIRE(std::string(detail)=="none");
}
TEST(gps_whole_second_json_number_notation_keeps_exact_source_deadline) {
    for(const char* number:{"1800000000.0","1.8e9","1800000000000e-3"}) {
        Observation o;const char* detail=nullptr;
        REQUIRE(parse_vehicle(body(std::string("\"latitude\":0,\"longitude\":0,\"gps_as_of\":")+number),config().vin,true,o,&detail)==Error::None);
        REQUIRE(o.source_s==epoch);REQUIRE(std::string(detail)=="gps_as_of_numeric_seconds");
        o.generation=1;o.request=1;
        Policy p;p.configure(config(),1,0);p.observe(o,0,epoch,true);
        REQUIRE(p.tick(899999).auto_home);REQUIRE(!p.tick(900000).auto_home);
    }
}
TEST(gps_wire_diagnostic_preserves_only_bounded_numeric_text) {
    for(const char* wire:{"-823000000.0","1.8e9","1800000000","1800000000000e-3"}) {
        Observation o;double parsed=0;char text[64]={};
        auto result=parse_vehicle(body(std::string("\"latitude\":0,\"longitude\":0,\"gps_as_of\":")+wire),
                                  config().vin,true,o,nullptr,&parsed,text,sizeof text);
        REQUIRE(std::string(text)==wire);
        if(wire[0]=='-') {REQUIRE(result==Error::Malformed);REQUIRE(o.kind==Evidence::Unknown);REQUIRE(parsed<0);}
        else {REQUIRE(result==Error::None);REQUIRE(o.source_s==epoch);}
    }
    for(const auto& wire:{std::string("null"),std::string("\"private upstream text\""),std::string(80,'9')}) {
        Observation o;char text[64]="old value";
        REQUIRE(parse_vehicle(body("\"latitude\":0,\"longitude\":0,\"gps_as_of\":"+wire),
                              config().vin,true,o,nullptr,nullptr,text,sizeof text)==Error::Malformed);
        REQUIRE(text[0]==0);
    }
    Json j;char small[3]="xx";REQUIRE(j.parse("123"));
    REQUIRE(!j.number_text(0,small,sizeof small));REQUIRE(small[0]==0);
    REQUIRE(!j.number_text(0,nullptr,0));
}
TEST(report_metadata_never_substitutes_for_gps_source_time) {
    Observation o;VehicleMetadata m;
    auto wire=body("\"latitude\":0,\"longitude\":0,\"gps_as_of\":-123456789,\"timestamp\":1800000000123");
    auto at=wire.find("\"state\"");wire.insert(at,"\"api_version\":83,");
    REQUIRE(parse_vehicle(wire,config().vin,true,o,nullptr,nullptr,nullptr,0,&m)==Error::Malformed);
    REQUIRE(std::string(m.report_timestamp_text)=="1800000000123");REQUIRE(m.api_version==83);
    REQUIRE(o.kind==Evidence::Unknown);
    o.generation=1;o.request=1;Policy p;p.configure(config(),1,0);p.observe(o,30000,epoch,true);
    REQUIRE(!p.tick(30000).commanded);REQUIRE(!p.tick(30000).auto_home);
    for(const auto& value:{std::string("null"),std::string("\"private text\""),std::string(80,'9')}) {
        wire=body("\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000,\"timestamp\":"+value);
        REQUIRE(parse_vehicle(wire,config().vin,true,o,nullptr,nullptr,nullptr,0,&m)==Error::None);
        REQUIRE(m.report_timestamp_text[0]==0);REQUIRE(m.api_version==-1);
        REQUIRE(o.source_s==epoch);
    }
}
TEST(json_bounds_duplicates_and_hostile_input) {
    Json j;
    for(auto s:{"{\"vin\":1,\"vin\":2}","{\"v\\u0069n\":1}","[01]","[NaN]","{}garbage","{\"a\":}","[1,]","\"unterminated","[1e]"})REQUIRE(!j.parse(s));
    REQUIRE(!j.parse(std::string(14,'[')+"0"+std::string(14,']')));
    REQUIRE(!j.parse(std::string(17000,' ')));
    REQUIRE(j.parse("{\"big\":9007199254740993,\"x\":null,\"zero\":0}"));
    int64_t v=0;REQUIRE(j.integer(j.get(0,"big"),v));REQUIRE(v==9007199254740993LL);
    double d=1;REQUIRE(!j.number(j.get(0,"x"),d));REQUIRE(j.number(j.get(0,"zero"),d));REQUIRE(d==0);
}
TEST(unicode_and_escaped_secret_roundtrip) {
    Json j;REQUIRE(j.parse("{\"s\":\"caf\\u00e9\\n\\uD83D\\uDE00\"}"));char text[32]={};
    REQUIRE(j.string(j.get(0,"s"),text,sizeof text));REQUIRE(std::string(text)=="café\n😀");
    REQUIRE(j.parse("{\"s\":\"\\u0000\"}"));REQUIRE(!j.string(j.get(0,"s"),text,sizeof text));
}
TEST(tokens_expiry_scopes_and_redacted_error_categories) {
    Tokens t;
    std::string b="{\"access_token\":\"synthetic-access\",\"refresh_token\":\"synthetic-refresh\",\"token_type\":\"Bearer\",\"expires_in\":3210,\"scope\":\"openid offline_access vehicle_device_data vehicle_location\"}";
    REQUIRE(parse_tokens(b,t)==Error::None);REQUIRE(t.expires_s==3210);
    auto p=b.find("vehicle_location");b.erase(p,16);REQUIRE(parse_tokens(b,t)==Error::Permission);
    REQUIRE(parse_tokens("{\"error\":\"login_required\"}",t)==Error::Reauthorize);
    REQUIRE(http_error(401)==Error::Authentication);REQUIRE(http_error(403)==Error::Permission);
    REQUIRE(http_error(429)==Error::RateLimit);REQUIRE(http_error(402)==Error::Billing);
    REQUIRE(http_error(503)==Error::Server);REQUIRE(http_error(408)==Error::Timeout);
    REQUIRE(http_error(302)==Error::Redirect);REQUIRE(http_error(405)==Error::Unavailable);
    REQUIRE(std::string(error_name(Error::Authentication))=="authentication");
}
TEST(chunked_decoded_body_and_oversize_guard) {
    BodyBuffer buffer;REQUIRE(buffer.append("{\"response\":",12));REQUIRE(buffer.append("null}",5));
    Json j;REQUIRE(j.parse(buffer.view()));
    std::string too_big(16384,'x');REQUIRE(!buffer.append(too_big.data(),too_big.size()));REQUIRE(buffer.overflow());
    REQUIRE(!buffer.append("a",1));
}
TEST(configuration_validation) {
    auto c=config();REQUIRE(valid_config(c));c.dwell_s=29;REQUIRE(!valid_config(c));
    c=config();c.enable_m=c.disable_m;REQUIRE(!valid_config(c));
    c=config();c.region=2;REQUIRE(!valid_config(c));c=config();c.home_lat=91;REQUIRE(!valid_config(c));
    c=config();c.lease_s=901;REQUIRE(!valid_config(c));c=config();c.monthly_cap=0;REQUIRE(!valid_config(c));
    c=config();c.vin[17]='X';REQUIRE(!valid_config(c));
}

#include "http_decoder.hpp"
TEST(http_support_metadata_is_bounded_unambiguous_and_json_safe) {
    const std::string date="Wed, 23 Sep 2026 10:56:00 GMT";
    auto decode=[&](const std::string& headers,const char* expected_id,const char* expected_date) {
        BodyBuffer body;HttpDecoder d(body);
        auto wire="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"+headers+"\r\n{}";
        for(char c:wire)REQUIRE(d.feed(&c,1));
        REQUIRE(d.done());REQUIRE(d.body()=="{}");
        REQUIRE(std::string(d.transaction_id())==expected_id);
        REQUIRE(std::string(d.response_date())==expected_date);
    };
    decode("X-TxId: synthetic-1234_ab.cd:01\r\nDate: "+date+"\r\n","synthetic-1234_ab.cd:01",date.c_str());
    decode("X-TxId: first\r\nX-TxId: second\r\nDate: "+date+"\r\nDate: "+date+"\r\n","","");
    for(const auto& bad:{std::string("private\"text"),std::string("private\\text"),std::string(129,'a')})
        decode("X-TxId: "+bad+"\r\n","","");
    decode("Date: private\"text\r\n","","");
    BodyBuffer body;HttpDecoder d(body);
    std::string wire="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-TxId: original\r\n\r\n2\r\n{}\r\n0\r\nX-TxId: replacement\r\n\r\n";
    REQUIRE(d.feed(wire.data(),wire.size()));REQUIRE(d.done());
    REQUIRE(std::string(d.transaction_id())=="original");
}
TEST(http_chunked_arbitrary_fragments_and_retry_after) {
    std::string wire="HTTP/1.1 429 Too Many Requests\r\nTransfer-Encoding: chunked\r\nRetry-After: 120\r\n\r\n4\r\n{\"a\"\r\n3;ok=x\r\n:1}\r\n0\r\n\r\n";
    BodyBuffer d_body;HttpDecoder d(d_body);
    for(char c:wire)REQUIRE(d.feed(&c,1));
    REQUIRE(d.done());REQUIRE(d.status()==429);REQUIRE(d.body()=="{\"a\":1}");REQUIRE(std::string(d.retry_after())=="120");
}
TEST(http_malformed_truncation_overflow_and_smuggling_rejected) {
    for(auto wire:{"HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n",
                   "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na",
                   "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n",
                   "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n"}) {
        BodyBuffer d_body;HttpDecoder d(d_body);REQUIRE(!d.feed(wire,std::strlen(wire)));
    }
    BodyBuffer short_body_body;HttpDecoder short_body(short_body_body);std::string b="HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc";
    REQUIRE(short_body.feed(b.data(),b.size()));REQUIRE(!short_body.eof());
    BodyBuffer huge_body;HttpDecoder huge(huge_body);std::string close="HTTP/1.1 200 OK\r\n\r\n"+std::string(16385,'a');
    REQUIRE(!huge.feed(close.data(),close.size()));REQUIRE(huge.error()==Error::TooLarge);
}
TEST(http_fixed_and_close_delimited_responses) {
    for(auto wire:{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}","HTTP/1.0 200 OK\r\n\r\n{}"}) {
        BodyBuffer d_body;HttpDecoder d(d_body);REQUIRE(d.feed(wire,std::strlen(wire)));REQUIRE(d.eof());REQUIRE(d.body()=="{}");
    }
}
TEST(bearer_header_injection_rejected) {
    Tokens t;
    REQUIRE(parse_tokens("{\"access_token\":\"bad\\r\\nInjected: true\",\"refresh_token\":\"synthetic\",\"token_type\":\"Bearer\",\"expires_in\":900}",t)==Error::Malformed);
}
TEST(bounded_parser_mutation_corpus_no_crashes) {
    uint32_t random=0x31415926;
    auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    const std::string seed=body("\"latitude\":0,\"longitude\":0,\"gps_as_of\":1800000000");
    for(int i=0;i<10000;++i) {
        std::string s=seed;
        for(unsigned j=0,n=1+next()%8;j<n;++j)s[next()%s.size()]=char(next()%256);
        if(next()%4==0)s.resize(next()%s.size());
        Json json;
        if(json.parse(s)) {double v=0;int64_t integer=0;char buf[128];
            json.number(json.get(0,"response"),v);json.integer(json.get(0,"response"),integer);
            json.string(json.get(0,"response"),buf,sizeof buf);}
        Observation o;parse_vehicle(s,config().vin,true,o);
        BodyBuffer out;HttpDecoder decoder(out);decoder.feed(s.data(),s.size());decoder.eof();
    }
}
