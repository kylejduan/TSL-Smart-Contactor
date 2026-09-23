#pragma once
#include "runtime.hpp"
#include "client.hpp"
namespace app {
// USB-only passive diagnostic; rejects configured/armed devices.
void scan_unprovisioned_wifi(char* output,size_t capacity);
// URL is constructed internally from a fixed official host and a VIN. Never takes
// arbitrary network destinations from web handlers or provisioning.
void request(Endpoint,const Config&,const char* access,const char* form,HttpResult& result);
}
