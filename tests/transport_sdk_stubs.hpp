#pragma once
#include "runtime_sdk_stubs.hpp"
#include <sys/types.h>
struct esp_tls_t {int unused;};
enum esp_tls_conn_state_t {ESP_TLS_INIT,ESP_TLS_CONNECTING,ESP_TLS_HANDSHAKE,ESP_TLS_FAIL,ESP_TLS_DONE};
using esp_tls_error_handle_t=void*;
enum esp_tls_error_type_t {ESP_TLS_ERR_TYPE_SYSTEM};
struct esp_tls_cfg_t {
    esp_err_t (*crt_bundle_attach)(void*)=nullptr;
    bool non_block=false;
    int timeout_ms=0;
};
inline constexpr int ESP_TLS_ERR_SSL_WANT_READ=-0x6900,ESP_TLS_ERR_SSL_WANT_WRITE=-0x6880;
inline constexpr int ESP_ERR_INVALID_STATE=0x103,ESP_ERR_NO_MEM=0x101,ERR_OK=0,ERR_MEM=-1;
using tcpip_callback_fn=void (*)(void*);
int tcpip_callback(tcpip_callback_fn,void*);
void dns_clear_cache();
esp_err_t esp_wifi_disconnect();
esp_err_t esp_crt_bundle_attach(void*);
esp_tls_t* esp_tls_init();
int esp_tls_conn_new_async(const char*,int,int,const esp_tls_cfg_t*,esp_tls_t*);
ssize_t esp_tls_conn_write(esp_tls_t*,const void*,size_t);
ssize_t esp_tls_conn_read(esp_tls_t*,void*,size_t);
esp_err_t esp_tls_get_conn_state(esp_tls_t*,esp_tls_conn_state_t*);
esp_err_t esp_tls_get_error_handle(esp_tls_t*,esp_tls_error_handle_t*);
esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t,esp_tls_error_type_t,int*);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t,int*,int*);
int esp_tls_conn_destroy(esp_tls_t*);
void mbedtls_platform_zeroize(void*,size_t);
