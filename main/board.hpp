#pragma once
#include "driver/gpio.h"
namespace board {
// Waveshare ESP32-S3-Relay-1CH, SKU 32152; schematic and vendor WS_GPIO.h.
inline constexpr gpio_num_t relay_pin = GPIO_NUM_47;
inline constexpr int relay_on = 1, relay_off = 0;
// Native USB Serial/JTAG: GPIO19 D- / GPIO20 D+. No other peripheral pins used.
inline bool initialize_off() {
    if (gpio_set_level(relay_pin, relay_off) != ESP_OK) return false;
    return gpio_set_direction(relay_pin, GPIO_MODE_OUTPUT) == ESP_OK;
}
inline bool command(bool on) { return gpio_set_level(relay_pin,on ? relay_on : relay_off)==ESP_OK; }
}
