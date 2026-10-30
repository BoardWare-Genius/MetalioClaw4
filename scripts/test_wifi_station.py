#!/usr/bin/env python3
"""Run the real WifiStation against host-side ESP-IDF fault-injection stubs.

Requires Python 3 and g++; no attached device or ESP-IDF environment is needed.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/78__esp-wifi-connect"

STUB = r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=1;
constexpr int ESP_ERR_INVALID_STATE=4;
constexpr int ESP_ERR_TIMEOUT=5;
constexpr int ESP_ERR_WIFI_NOT_INIT=2, ESP_ERR_WIFI_NOT_STARTED=3;
constexpr int WIFI_MODE_STA=1, WIFI_IF_STA=0, WIFI_PS_MIN_MODEM=1, WIFI_PS_NONE=0;
constexpr int WIFI_EVENT=1, IP_EVENT=2, ESP_EVENT_ANY_ID=-1, IP_EVENT_STA_GOT_IP=1;
constexpr int WIFI_EVENT_STA_START=1, WIFI_EVENT_SCAN_DONE=2;
constexpr int WIFI_EVENT_STA_DISCONNECTED=3, WIFI_EVENT_STA_CONNECTED=4;
constexpr int ESP_TIMER_TASK=0, BIT0=1, pdFALSE=0, portTICK_PERIOD_MS=1;
constexpr int BIT1=2, BIT2=4, ESP_HOSTED_EVENT=3;
constexpr int ESP_HOSTED_EVENT_CP_INIT=0, ESP_HOSTED_EVENT_TRANSPORT_FAILURE=2;
constexpr int ESP_HOSTED_EVENT_TRANSPORT_DOWN=4;
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
inline const char* esp_err_to_name(int) { return "injected"; }
using EventGroupHandle_t = unsigned*;
inline auto xEventGroupCreate() { return new unsigned(0); }
inline void vEventGroupDelete(EventGroupHandle_t p) { delete p; }
inline unsigned xEventGroupGetBits(EventGroupHandle_t p) { return *p; }
inline void xEventGroupClearBits(EventGroupHandle_t p, unsigned b) { *p &= ~b; }
inline void xEventGroupSetBits(EventGroupHandle_t p, unsigned b) { *p |= b; }
inline int waits=0, delays=0;
inline void (*wait_hook)()=nullptr;
inline unsigned xEventGroupWaitBits(EventGroupHandle_t p,unsigned,int,int,int) {
    ++waits;
    if (wait_hook) { auto hook=wait_hook; wait_hook=nullptr; hook(); }
    return *p;
}
inline void vTaskDelay(int) { ++delays; }
using esp_event_base_t = int;
using Callback = void(*)(void*,int,int,void*);
struct Handler { int base; Callback callback; void* arg; };
using esp_event_handler_instance_t = Handler*;
inline std::vector<Handler*> handlers;
inline std::string fail_stage;
inline bool hosted_reply=true, hosted_boot_ready=false;
inline int hosted_connects=0;
inline int esp_event_handler_instance_register(int base,int,Callback cb,void* arg,Handler** out) {
    if (fail_stage == (base==WIFI_EVENT ? "wifi_handler" : "ip_handler")) return ESP_FAIL;
    *out = new Handler{base,cb,arg}; handlers.push_back(*out); return ESP_OK;
}
inline int esp_event_handler_instance_unregister(int,int,Handler* p) {
    if (fail_stage=="unregister") return ESP_FAIL;
    auto it=std::find(handlers.begin(),handlers.end(),p); assert(it!=handlers.end());
    handlers.erase(it); delete p; return ESP_OK;
}
inline void emit(int base,int event,void* data=nullptr) {
    if (base==ESP_HOSTED_EVENT) hosted_boot_ready=(event==ESP_HOSTED_EVENT_CP_INIT);
    for (auto h : handlers) if (h->base==base) h->callback(h->arg,base,event,data);
}
inline int esp_hosted_connect_to_slave() {
    ++hosted_connects;
    // Only deliver the boot event during the wait, after link setup returns.
    if (hosted_reply) wait_hook=[] { emit(ESP_HOSTED_EVENT,ESP_HOSTED_EVENT_CP_INIT); };
    return ESP_OK;
}
struct esp_timer_create_args_t {
    void (*callback)(void*); void* arg; int dispatch_method; const char* name; bool skip_unhandled_events;
};
using esp_timer_handle_t = esp_timer_create_args_t*;
inline int timers=0;
inline uint64_t scheduled_delay=0;
inline int esp_timer_create(const esp_timer_create_args_t* a,esp_timer_handle_t* p) {
    if (fail_stage=="timer") return ESP_FAIL;
    *p=new esp_timer_create_args_t(*a); ++timers; return ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t p) { assert(p); return ESP_OK; }
inline int esp_timer_delete(esp_timer_handle_t p) { assert(p); delete p; --timers; return ESP_OK; }
inline int esp_timer_start_once(esp_timer_handle_t p,uint64_t us) { assert(p); scheduled_delay=us; return ESP_OK; }
struct esp_netif_t {};
inline int netifs=0;
inline int esp_netif_init() { return ESP_OK; }
inline int esp_event_loop_create_default() { return ESP_OK; }
inline esp_netif_t* esp_netif_create_default_wifi_sta() { assert(netifs==0); ++netifs; return new esp_netif_t; }
inline void esp_netif_destroy_default_wifi(esp_netif_t* p) { delete p; --netifs; }
inline void esp_ip4addr_ntoa(const void*,char* p,size_t n) { snprintf(p,n,"192.0.2.1"); }
using wifi_auth_mode_t=int;
struct wifi_ap_record_t { uint8_t ssid[33]; uint8_t bssid[6]; int8_t rssi; uint8_t primary; int authmode; };
struct wifi_init_config_t { bool nvs_enable; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t { struct { uint8_t ssid[32]; uint8_t password[64]; int channel; uint8_t bssid[6]; bool bssid_set; } sta; };
struct ip_event_got_ip_t { struct { uint32_t ip; } ip_info; };
inline std::vector<int> init_results;
inline int init_calls=0, deinit_calls=0, start_calls=0, stop_calls=0, scan_calls=0;
inline bool deinit_fails=false, ap_info_fails=false;
inline bool expect_timer=true;
inline uint16_t ap_count=0;
inline int esp_wifi_init(const wifi_init_config_t*) {
#if CONFIG_ESP_HOSTED_ENABLED
    assert(hosted_boot_ready);
#endif
    ++init_calls;
    if (init_results.empty()) return ESP_OK;
    int result=init_results.front(); init_results.erase(init_results.begin()); return result;
}
inline int esp_wifi_deinit() { ++deinit_calls; return deinit_fails ? ESP_FAIL : ESP_OK; }
inline int esp_wifi_stop() { ++stop_calls; return ESP_ERR_WIFI_NOT_STARTED; }
inline int esp_wifi_scan_stop() { return ESP_OK; }
inline int esp_wifi_set_mode(int) { return fail_stage=="mode" ? ESP_FAIL : ESP_OK; }
inline int esp_wifi_start() {
    ++start_calls;
    if (fail_stage=="start") return ESP_FAIL;
    assert(!expect_timer || timers==1); emit(WIFI_EVENT,WIFI_EVENT_STA_START); return ESP_OK;
}
inline int esp_wifi_scan_start(void*,bool) { ++scan_calls; return ESP_OK; }
inline int esp_wifi_scan_get_ap_num(uint16_t* n) { *n=ap_count; return ESP_OK; }
inline int esp_wifi_scan_get_ap_records(uint16_t* n,wifi_ap_record_t* p) {
    for (int i=0;i<*n;++i) { p[i]={}; strcpy((char*)p[i].ssid,"test-ap"); }
    return ESP_OK;
}
inline int esp_wifi_set_config(int,wifi_config_t*) { return fail_stage=="config" ? ESP_FAIL : ESP_OK; }
inline int esp_wifi_connect() { return fail_stage=="connect" ? ESP_FAIL : ESP_OK; }
inline int esp_wifi_set_max_tx_power(int8_t) { return ESP_OK; }
inline int esp_wifi_set_ps(int) { return ESP_FAIL; }
inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t* p) {
    if (ap_info_fails) return ESP_FAIL;
    p->rssi=-50; p->primary=6; return ESP_OK;
}
using nvs_handle_t = int;
constexpr int NVS_READONLY=0;
inline int nvs_open(const char*,int,int*) { return ESP_FAIL; }
inline int nvs_get_i8(int,const char*,int8_t*) { abort(); }
inline int nvs_get_u8(int,const char*,uint8_t*) { abort(); }
inline void nvs_close(int) { abort(); }
'''

HOSTED_HARNESS = r'''
#include "wifi_station.h"
#include "ssid_manager.h"
#include "esp_mock.h"
SsidManager::SsidManager() {}
SsidManager::~SsidManager() {}
void SsidManager::AddSsid(const std::string&,const std::string&) {}
int main() {
    auto& station=WifiStation::GetInstance();
    hosted_reply=false;
    assert(station.PrepareHostedWifi()==ESP_ERR_TIMEOUT);
    assert(init_calls==0); // never initialize WiFi while only the SDIO link is up
    hosted_reply=true;
    assert(station.Start());
    assert(hosted_connects==2 && hosted_boot_ready && init_calls==1);
    ip_event_got_ip_t ip{}; emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(station.IsConnected());
    emit(ESP_HOSTED_EVENT,ESP_HOSTED_EVENT_TRANSPORT_FAILURE);
    assert(!station.IsConnected() && !station.IsStarted());
    assert(station.PrepareHostedWifi()==ESP_FAIL && hosted_connects==2);
    assert(station.Stop());
    // A fresh C5 boot event permits a later manual retry.
    emit(ESP_HOSTED_EVENT,ESP_HOSTED_EVENT_CP_INIT);
    assert(station.Start());
    assert(station.Stop());
    assert(netifs==0 && timers==0 && handlers.size()==1);
    puts("PASS: C5 boot barrier, readiness timeout, transport failure invalidates connectivity, later C5 boot permits retry");
}
'''

HARNESS = r'''
#include "wifi_station.h"
#include "ssid_manager.h"
#include "esp_mock.h"
SsidManager::SsidManager() {}
SsidManager::~SsidManager() {}
void SsidManager::AddSsid(const std::string& ssid,const std::string& password) {
    ssid_list_.push_back({ssid,password});
}
void reset() {
    auto& station=WifiStation::GetInstance();
    assert(station.Stop());
    assert(netifs==0 && timers==0 && handlers.empty());
    init_calls=deinit_calls=start_calls=stop_calls=scan_calls=waits=delays=0;
    scheduled_delay=0; fail_stage.clear(); init_results.clear(); ap_count=0;
}
// MANUAL_SCREEN_TESTS
int main() {
    auto& station=WifiStation::GetInstance();
    reset();
    // The observed crash: init times out once, rollback then retry succeeds.
    init_results={ESP_FAIL,ESP_OK};
    assert(station.Start());
    assert(init_calls==2 && deinit_calls==1 && delays==1);
    assert(station.Start() && init_calls==2); // no duplicate netif/timer
    assert(station.Stop() && station.Stop());
    assert(netifs==0 && timers==0 && handlers.empty());

    reset();
    init_results={ESP_FAIL,ESP_FAIL,ESP_FAIL};
    assert(!station.Start());
    assert(init_calls==3 && deinit_calls==3 && delays==2);
    assert(!station.WaitForConnected(60000) && waits==0);
    assert(netifs==0 && timers==0 && handlers.empty());
    assert(station.Start()); // manual recovery after exhausting startup retries

    reset();
    // A remote init can complete even when its reply was lost. If deinit also
    // fails, retain ownership and do not attempt a duplicate initialization.
    init_results={ESP_FAIL}; deinit_fails=true;
    assert(!station.Start() && init_calls==1 && netifs==1);
    assert(!station.Start() && init_calls==1);
    deinit_fails=false;
    assert(station.Start() && init_calls==2);

    for (const auto* stage : {"wifi_handler","ip_handler","timer","mode","start"}) {
        reset(); fail_stage=stage;
        assert(!station.Start());
        assert(init_calls==3 && netifs==0 && timers==0 && handlers.empty());
        fail_stage.clear(); assert(station.Start());
    }

    reset(); assert(station.Start());
    emit(WIFI_EVENT,WIFI_EVENT_SCAN_DONE); // no AP: bounded scan rate, valid timer
    assert(scheduled_delay==10000000);
    ip_event_got_ip_t ip{}; emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(station.IsConnected() && station.GetRssi()==-50 && station.GetChannel()==6);
    ap_info_fails=true;
    assert(station.GetRssi()==-127 && station.GetChannel()==0); // disconnect race
    station.SetPowerSaveMode(true); // power-save errors must not reboot either
    ap_info_fails=false;
    const Handler late=*handlers.front();
    assert(station.Stop());
    int scans_before=scan_calls;
    late.callback(late.arg,WIFI_EVENT,WIFI_EVENT_STA_START,nullptr);
    assert(scan_calls==scans_before && !station.IsConnected());

    reset(); assert(station.Start()); fail_stage="unregister";
    assert(!station.Stop());
    assert(!station.Start() && init_calls==1);
    fail_stage.clear(); assert(station.Start());

    reset(); assert(station.Start());
    SsidManager::GetInstance().AddSsid("test-ap","password");
    ap_count=1; fail_stage="config";
    emit(WIFI_EVENT,WIFI_EVENT_SCAN_DONE);
    assert(scheduled_delay==10000000);
    fail_stage="connect"; scheduled_delay=0;
    emit(WIFI_EVENT,WIFI_EVENT_SCAN_DONE);
    assert(scheduled_delay==10000000);
    fail_stage.clear(); reset();
    network_screen::test();
    wifi_test::test();
    puts("PASS: startup retry/rollback, persistent timeout, cleanup failure, restart, partial setup, disconnect races, scan/connect errors, both manual WiFi screens");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="metalio-wifi-test-") as tmp:
        path = Path(tmp)
        (path / "esp_mock.h").write_text(STUB)
        headers = [
            "freertos/FreeRTOS.h", "freertos/event_groups.h", "freertos/task.h",
            "esp_log.h", "esp_wifi.h", "esp_wifi_types_generic.h", "esp_event.h",
            "esp_timer.h", "esp_netif.h", "esp_system.h", "nvs.h", "nvs_flash.h",
            "esp_hosted.h",
        ]
        for name in headers:
            header = path / name
            header.parent.mkdir(exist_ok=True, parents=True)
            header.write_text('#include "esp_mock.h"\n')
        harness = path / "test.cc"
        manual_tests = []
        for namespace, source, init, teardown, callback in [
            ("network_screen", "network_screen/network_screen.cc", "wifi_init_for_screen",
             "wifi_teardown_for_screen", "wifi_evt_handler"),
            ("wifi_test", "test_screen/wifi_test.cc", "WifiInitForTest",
             "WifiTeardownForTest", "WifiEvtHandler"),
        ]:
            # Compile the actual non-UI lifecycle functions, not a test copy.
            source_text = (ROOT / "main/display/screen" / source).read_text()
            begin = source_text.index("bool CleanupWifiStack() {")
            end = source_text.index("\n}\n", source_text.index(f"void {teardown}()", begin)) + 3
            lifecycle = source_text[begin:end]
            manual_tests.append(f'''
namespace {namespace} {{
constexpr const char* TAG="manual-test";
bool s_wifi_initialized=false, s_wifi_station_was_active=false;
EventGroupHandle_t s_evt_group=nullptr;
esp_netif_t* s_netif=nullptr;
esp_event_handler_instance_t s_wifi_evt_inst=nullptr, s_ip_evt_inst=nullptr;
void {callback}(void*,int,int,void*) {{}}
{lifecycle}
void test() {{
    reset(); expect_timer=false;
    init_results={{ESP_FAIL}};
    assert(!{init}());
    assert(netifs==0 && handlers.empty());
    assert({init}());
    {teardown}(); {teardown}();
    assert(netifs==0 && handlers.empty());
    for (const auto* stage : {{"wifi_handler","ip_handler","mode","start"}}) {{
        fail_stage=stage;
        assert(!{init}());
        assert(netifs==0 && handlers.empty());
        fail_stage.clear(); assert({init}()); {teardown}();
    }}
    init_results={{ESP_FAIL}}; deinit_fails=true;
    assert(!{init}() && netifs==1);
    assert(!{init}() && netifs==1);
    deinit_fails=false;
    assert({init}()); {teardown}();
    expect_timer=true;
    assert(WifiStation::GetInstance().Start());
    expect_timer=false;
    assert({init}()); // manual scan takes over the existing station
    expect_timer=true;
    {teardown}(); // exiting restores the original station
    assert(WifiStation::GetInstance().IsStarted());
    vEventGroupDelete(s_evt_group); s_evt_group=nullptr;
    reset();
}}
}}
''')
        harness.write_text(HARNESS.replace("// MANUAL_SCREEN_TESTS", "\n".join(manual_tests)))
        binary = path / "wifi_station_test"
        subprocess.run([
            "g++", "-std=c++17", "-g", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-pthread", f"-I{path}",
            f"-I{COMPONENT / 'include'}", str(COMPONENT / "wifi_station.cc"),
            str(harness), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        harness.write_text(HOSTED_HARNESS)
        subprocess.run([
            "g++", "-std=c++17", "-g", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-pthread", "-DCONFIG_ESP_HOSTED_ENABLED=1",
            f"-I{path}", f"-I{COMPONENT / 'include'}",
            str(COMPONENT / "wifi_station.cc"), str(harness), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
