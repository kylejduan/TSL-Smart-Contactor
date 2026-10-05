#include "board.hpp"
#include "bench_protocol.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>

namespace {
std::atomic<bool> allocation_failure{false};
void allocation_failed(std::size_t,std::uint32_t,const char*) {allocation_failure=true;}
bench::Ms now_ms() {return static_cast<bench::Ms>(esp_timer_get_time()/1000);}
void drive(bench::Session& session) {
    if(allocation_failure)session.fail();
    if(!board::command(session.on())) {
        session.fail();
        (void)board::command(false);
    }
}
void respond(bench::Session& session,bench::Reply reply) {
    char message[320];
    const int n=std::snprintf(message,sizeof message,
        "{\"bench\":\"TSL_USB_RELAY_BENCH\",\"protocol\":1,\"nonce\":\"%s\","
        "\"dwell_remaining_ms\":%llu,\"commanded_on\":%s,\"used\":%s,\"fault\":%s,"
        "\"ok\":%s,\"result\":\"%s\"}\n",
        session.nonce(),static_cast<unsigned long long>(session.dwell_remaining(now_ms())),
        session.on()?"true":"false",session.used()?"true":"false",session.fault()?"true":"false",
        bench::accepted(reply)?"true":"false",bench::reply_name(reply));
    // No wait for a host reader. A failed/partial write inhibits this boot.
    if(n<=0 || static_cast<std::size_t>(n)>=sizeof message ||
       usb_serial_jtag_write_bytes(message,static_cast<std::size_t>(n),0)!=n) {
        session.fail();drive(session);
    }
}
}
extern "C" void app_main() {
    // First hardware action: inactive latch before output driver. app_main is
    // the only GPIO owner; even USB parsing happens in this control loop.
    const bool gpio_ready=board::initialize_off();
    const bench::Ms boot=now_ms();
    std::uint8_t random[16];char nonce[33]={};
    esp_fill_random(random,sizeof random);
    constexpr char hex[]="0123456789abcdef";
    for(std::size_t i=0;i<sizeof random;++i) {
        nonce[2*i]=hex[random[i]>>4];nonce[2*i+1]=hex[random[i]&15];
    }
    bench::Session session(boot,nonce);
    if(!gpio_ready)session.fail();
    vTaskPrioritySet(nullptr,10);
    if(heap_caps_register_failed_alloc_callback(allocation_failed)!=ESP_OK)session.fail();
    const esp_task_wdt_config_t watchdog={.timeout_ms=1000,.idle_core_mask=0,.trigger_panic=true};
    esp_err_t wd=esp_task_wdt_reconfigure(&watchdog);
    if(wd==ESP_ERR_INVALID_STATE)wd=esp_task_wdt_init(&watchdog);
    const bool watchdog_ready=wd==ESP_OK && esp_task_wdt_add(nullptr)==ESP_OK;
    if(!watchdog_ready)session.fail();
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=1024,.rx_buffer_size=256};
    const bool usb_ready=usb_serial_jtag_driver_install(&usb)==ESP_OK;
    if(!usb_ready)session.fail();
    drive(session);
    TickType_t wake=xTaskGetTickCount();
    bench::Ms previous=now_ms();
    while(true) {
        const bench::Ms now=now_ms();
        if(now<previous || now-previous>20)session.fail();
        previous=now;
        session.tick(now);drive(session);
        if(usb_ready) {
            char input[256];
            const int count=usb_serial_jtag_read_bytes(input,sizeof input,0);
            bench::Reply reply=bench::Reply::None;
            if(count<0 || count>static_cast<int>(sizeof input))session.fail();
            else for(int i=0;i<count;++i) {
                const auto result=session.receive(input[i],now_ms());
                if(result!=bench::Reply::None)reply=result;
            }
            // Drain the whole bounded batch before output/response: OFF behind
            // other commands wins. Multiple commands in a batch get one final
            // status response; the host protocol sends one command at a time.
            session.tick(now_ms());drive(session);
            if(reply!=bench::Reply::None)respond(session,reply);
        }
        session.tick(now_ms());drive(session);
        // Feed only after this same task has processed deadlines/input and GPIO.
        if(watchdog_ready && esp_task_wdt_reset()!=ESP_OK) {session.fail();drive(session);}
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(5));
    }
}
