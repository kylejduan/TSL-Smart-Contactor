cmake_minimum_required(VERSION 3.22)
project(controller_native LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(SANITIZE "Enable address/undefined sanitizers" ON)
add_library(controller ../components/controller/policy.cpp ../components/controller/auto_journal.cpp ../components/controller/json.cpp
    ../components/controller/protocol.cpp ../components/controller/reliability.cpp ../components/controller/http_decoder.cpp ../components/controller/client.cpp)
target_include_directories(controller PUBLIC ../components/controller/include)
target_compile_options(controller PRIVATE -Wall -Wextra -Werror -Wpedantic)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(controller PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(controller PUBLIC -fsanitize=address,undefined)
endif()
enable_testing()
add_executable(controller_tests test_main.cpp policy_tests.cpp outage_policy_tests.cpp auto_journal_tests.cpp protocol_tests.cpp reliability_tests.cpp client_tests.cpp control_tests.cpp autonomy_tests.cpp)
target_link_libraries(controller_tests PRIVATE controller)
target_compile_options(controller_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
add_test(NAME controller_tests COMMAND controller_tests)
add_executable(bench_relay_tests test_bench_relay.cpp)
target_include_directories(bench_relay_tests PRIVATE ../main)
target_compile_options(bench_relay_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_relay_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(bench_relay_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME bench_relay_tests COMMAND bench_relay_tests)

# Compile the production entrypoint and board adapter unchanged against test-only
# SDK declarations. GNU-compatible time wrapping supplies deterministic UTC.
# Each scenario is a separate process so no previous boot's RAM is retained.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(runtime_sdk_headers
        freertos/FreeRTOS.h freertos/semphr.h freertos/queue.h driver/gpio.h nvs.h
        esp_timer.h esp_task_wdt.h esp_heap_caps.h esp_psram.h esp_system.h esp_random.h)
    foreach(header IN LISTS runtime_sdk_headers)
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime-sdk/${header}"
            CONTENT "#include \"runtime_sdk_stubs.hpp\"\n")
    endforeach()
    add_executable(firmware_runtime_tests runtime_tests.cpp ../main/app.cpp ../main/auto_state.cpp)
    target_include_directories(firmware_runtime_tests PRIVATE . ../main "${CMAKE_CURRENT_BINARY_DIR}/runtime-sdk")
    target_link_libraries(firmware_runtime_tests PRIVATE controller)
    target_compile_options(firmware_runtime_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
    target_link_options(firmware_runtime_tests PRIVATE -Wl,--wrap=time)
    foreach(scenario boot armed_boot asleep_boot disabled uncommissioned dry_run
            latch_failure setup_failure usb_task_failure storage_failure allocation_monitor_failure
            queue_creation_failure psram_failure
            watchdog_add_failure queue_overflow lease_expiry gpio_failure off_during_io
            control_deadline watchdog_feed_failure recoverable_allocation)
        add_test(NAME runtime_${scenario} COMMAND firmware_runtime_tests ${scenario})
    endforeach()
    foreach(scenario held_boot held_asleep_boot held_outage held_off held_control_deadline)
        add_test(NAME runtime_${scenario} COMMAND firmware_runtime_tests ${scenario})
    endforeach()
    foreach(scenario held_saved_home held_saved_away held_saved_disabled held_saved_dry_run
            held_saved_watchdog held_saved_corrupt held_commit_blocked held_commit_failure
            held_off_pending held_storage_task_failure held_config_swap)
        add_test(NAME runtime_${scenario} COMMAND firmware_runtime_tests ${scenario})
    endforeach()
endif()
