idf_component_register(SRCS "policy.cpp" "json.cpp" "protocol.cpp" "reliability.cpp" "http_decoder.cpp" "client.cpp"
                       INCLUDE_DIRS "include")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
