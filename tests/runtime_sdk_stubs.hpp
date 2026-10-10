#pragma once
// Test-only declarations for compiling the unchanged production app.cpp and
// board.hpp on Linux. No stubs enter the ESP-IDF component or target firmware.
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = std::uint32_t;
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
using SemaphoreHandle_t = void*;
struct StaticSemaphore_t { int unused; };
struct StaticQueue_t { int unused; };
using QueueHandle_t = void*;
using nvs_handle_t = std::uint32_t;
using portMUX_TYPE = int;
inline constexpr portMUX_TYPE portMUX_INITIALIZER_UNLOCKED = 0;
inline constexpr BaseType_t pdTRUE = 1, pdPASS = 1, pdFALSE = 0;
inline constexpr TickType_t pdMS_TO_TICKS(std::uint32_t ms) { return ms; }
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
void runtime_unlock_hook(); // Test-only forced worker interleaving.
inline void portEXIT_CRITICAL(portMUX_TYPE*) {runtime_unlock_hook();}

enum gpio_num_t { GPIO_NUM_47 = 47 };
inline constexpr int GPIO_MODE_OUTPUT = 1;
esp_err_t gpio_set_level(gpio_num_t, int);
esp_err_t gpio_set_direction(gpio_num_t, int);
void vTaskPrioritySet(TaskHandle_t, UBaseType_t);
BaseType_t xTaskCreate(TaskFunction_t, const char*, std::uint32_t, void*, UBaseType_t, TaskHandle_t*);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, std::uint32_t, void*, UBaseType_t, TaskHandle_t*, BaseType_t);
void vTaskDelete(TaskHandle_t);
TickType_t xTaskGetTickCount();
void vTaskDelayUntil(TickType_t*, TickType_t);
void vTaskDelay(TickType_t);
QueueHandle_t xQueueCreateStatic(UBaseType_t, UBaseType_t, std::uint8_t*, StaticQueue_t*);
BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t);
std::int64_t esp_timer_get_time();
esp_err_t esp_task_wdt_add(TaskHandle_t);
esp_err_t esp_task_wdt_reset();
using AllocationCallback = void (*)(std::size_t, std::uint32_t, const char*);
esp_err_t heap_caps_register_failed_alloc_callback(AllocationCallback);
bool esp_psram_is_initialized();
std::size_t esp_psram_get_size();
std::uint32_t esp_random();
enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_TASK_WDT, ESP_RST_BROWNOUT, ESP_RST_PWR_GLITCH };
esp_reset_reason_t esp_reset_reason();

void esp_fill_random(void*,std::size_t);
