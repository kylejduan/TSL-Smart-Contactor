idf_component_register(SRCS "app.cpp" "storage.cpp" "network.cpp" "wifi_scan.cpp" "tesla.cpp" "management.cpp" "usb.cpp"
    INCLUDE_DIRS "." REQUIRES controller esp_driver_gpio esp_driver_usb_serial_jtag
    esp_psram esp_wifi esp_netif esp_event esp-tls esp_https_server nvs_flash mbedtls esp_timer esp_system esp_app_format lwip
    EMBED_TXTFILES "index.html" "style.css" "app.js")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
