#!/usr/bin/env python3
"""Opt-in host regression for the pinned SDK HTTPS allocation-failure cleanup.

Extracts the actual SDK httpd_ssl_open function, compiles it with narrow TLS/
HTTPD stubs, and injects transport-context allocation and handshake failures.
This checks ownership/error handling; it does not exercise real TLS or hardware.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
typedef void *httpd_handle_t;
typedef int esp_tls_error_handle_t;
typedef struct {int stub;} esp_tls_t;
typedef struct {int last_error, esp_tls_error_code, esp_tls_flags;} esp_https_server_last_error_t;
typedef struct {void *tls_cfg; int (*open_fn)(httpd_handle_t,int); void (*user_cb)(void *);} httpd_ssl_ctx_t;
typedef struct {esp_tls_t *tls; httpd_ssl_ctx_t *global_ctx;} httpd_ssl_transport_ctx_t;
typedef struct {int user_cb_state; esp_tls_t *tls;} esp_https_server_user_cb_arg_t;
enum {ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=257, HTTPS_SERVER_EVENT_ERROR=1,
      HTTPS_SERVER_EVENT_ON_CONNECTED=2, HTTPD_SSL_USER_CB_SESS_CREATE=3};
static int tls_live, deletes, handshake_result, error_events, connected_events, last_error;
static bool allocation_fails;
static httpd_ssl_ctx_t global;
static void *attached;
static void *httpd_get_global_transport_ctx(httpd_handle_t s) {(void)s;return &global;}
static esp_tls_t *esp_tls_init(void) {
    esp_tls_t *tls=malloc(sizeof(esp_tls_t));
    if(tls)++tls_live;
    return tls;
}
static int esp_tls_server_session_create(void *c,int fd,esp_tls_t *t) {
    (void)c;(void)fd;(void)t;return handshake_result;
}
static void esp_tls_server_session_delete(esp_tls_t *t) {
    assert(tls_live>0);--tls_live;++deletes;free(t);
}
static void http_dispatch_event_to_event_loop(int event,void *payload,size_t n) {
    (void)n;
    if(event==HTTPS_SERVER_EVENT_ERROR) {
        ++error_events;
        last_error=((esp_https_server_last_error_t *)payload)->last_error;
    } else if(event==HTTPS_SERVER_EVENT_ON_CONNECTED)++connected_events;
}
static int esp_tls_get_error_handle(esp_tls_t *t,esp_tls_error_handle_t *e) {
    (void)t;(void)e;return ESP_FAIL;
}
static int esp_tls_get_and_clear_last_error(esp_tls_error_handle_t e,int *a,int *b) {
    (void)e;(void)a;(void)b;return 0;
}
static void *context_calloc(size_t n,size_t size) {
    return allocation_fails ? NULL : calloc(n,size);
}
#define calloc context_calloc
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define httpd_sess_set_transport_ctx(s,fd,c,fn) (attached=(c))
#define httpd_sess_set_send_override(...) ((void)0)
#define httpd_sess_set_recv_override(...) ((void)0)
#define httpd_sess_set_pending_override(...) ((void)0)
"""

CHECKS = r"""
#define CHECK(condition, message) do { \
    if(!(condition)) {fputs("FAIL: " message "\n",stderr);return 1;} \
} while(0)
int main(void) {
    allocation_fails=true;
    CHECK(httpd_ssl_open(&global,5)==ESP_ERR_NO_MEM,"allocation error status changed");
    CHECK(error_events==1 && last_error==ESP_ERR_NO_MEM && connected_events==0,
          "allocation error notification changed");
    CHECK(attached==NULL && tls_live==0 && deletes==1,
          "transport allocation failure leaked or attached TLS ownership");
    puts("PASS: failed transport allocation frees TLS exactly once and retains NO_MEM");

    allocation_fails=false;handshake_result=-1;
    CHECK(httpd_ssl_open(&global,5)==ESP_FAIL,"failed handshake status changed");
    CHECK(attached==NULL && tls_live==0 && deletes==2 && connected_events==0,
          "failed handshake did not release TLS exactly once");
    puts("PASS: failed handshake releases TLS without attaching a session");

    handshake_result=0;
    CHECK(httpd_ssl_open(&global,5)==ESP_OK,"successful handshake rejected");
    CHECK(attached!=NULL && tls_live==1 && deletes==2 && connected_events==1,
          "successful session did not transfer TLS ownership to HTTPD");
    httpd_ssl_transport_ctx_t *context=attached;
    CHECK(context->tls!=NULL && context->global_ctx==&global,"attached context corrupted");
    esp_tls_server_session_delete(context->tls);free(context);
    CHECK(tls_live==0 && deletes==3,"successful session retained TLS after owner cleanup");
    puts("PASS: successful handshake transfers live TLS ownership to HTTPD");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path, help="Existing ESP-IDF v5.5.2 checkout")
    parser.add_argument("--cc", default="cc", help="Host C compiler executable (default: cc)")
    args = parser.parse_args()
    source_path = args.sdk / "components/esp_https_server/src/https_server.c"
    try:
        source = source_path.read_text(encoding="utf-8")
        start = source.index("static esp_err_t httpd_ssl_open(httpd_handle_t server, int sockfd)")
        end = source.index("\n/**\n * Tear down the HTTPD global transport context", start)
        function = source[start:end]
        with tempfile.TemporaryDirectory(prefix="tsl-sdk-https-cleanup-") as directory:
            temporary = Path(directory)
            c_file = temporary / "https_cleanup.c"
            executable = temporary / "https_cleanup"
            c_file.write_text(STUBS + function + CHECKS, encoding="utf-8")
            subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(c_file), "-o", str(executable)], check=True, timeout=30)
            subprocess.run([str(executable)], check=True, timeout=10)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"SDK HTTPS cleanup check failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
