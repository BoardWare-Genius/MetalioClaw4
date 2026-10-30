#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/task.h>
#include "ssid_manager.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define HOSTED_READY BIT1
#define HOSTED_FAILED BIT2
#define MAX_RECONNECT_COUNT 5

namespace {
constexpr int kStartAttempts = 3;
constexpr uint64_t kScanDelayUs = 10 * 1000 * 1000;

bool CheckWifiResult(esp_err_t err, const char* operation) {
    if (err == ESP_OK) return true;
    ESP_LOGW(TAG, "%s failed: %s (0x%x)", operation, esp_err_to_name(err), err);
    return false;
}
}

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();
#if CONFIG_ESP_HOSTED_ENABLED
    // Register before the first hosted connect, otherwise CP_INIT can be lost.
    CheckWifiResult(esp_event_handler_instance_register(
        ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, &WifiStation::HostedEventHandler,
        this, &instance_hosted_), "register hosted handler");
#endif

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
        max_tx_power_ = 0;
        remember_bssid_ = 0;
        return;
    }
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
    }
    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK) {
        remember_bssid_ = 0;
    }
    nvs_close(nvs);
}

WifiStation::~WifiStation() {
#if CONFIG_ESP_HOSTED_ENABLED
    if (instance_hosted_) {
        esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, instance_hosted_);
    }
#endif
    vEventGroupDelete(event_group_);
}

esp_err_t WifiStation::PrepareHostedWifi() {
#if CONFIG_ESP_HOSTED_ENABLED
    if (instance_hosted_ == nullptr) return ESP_FAIL;
    auto bits = xEventGroupGetBits(event_group_);
    if (bits & HOSTED_FAILED) return ESP_FAIL;
    if (bits & HOSTED_READY) return ESP_OK;
    auto err = esp_hosted_connect_to_slave();
    if (err != ESP_OK) return err;
    bits = xEventGroupWaitBits(event_group_, HOSTED_READY | HOSTED_FAILED,
                              pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & HOSTED_READY) || (bits & HOSTED_FAILED)) {
        ESP_LOGE(TAG, "C5 RPC service not ready; WiFi initialization deferred");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "C5 boot event received; initializing WiFi");
#endif
    return ESP_OK;
}

void WifiStation::HostedEventHandler(void* arg, esp_event_base_t, int32_t event_id, void*) {
#if CONFIG_ESP_HOSTED_ENABLED
    auto* station = static_cast<WifiStation*>(arg);
    if (event_id == ESP_HOSTED_EVENT_CP_INIT) {
        xEventGroupClearBits(station->event_group_, HOSTED_FAILED);
        xEventGroupSetBits(station->event_group_, HOSTED_READY);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE ||
               event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
        station->active_ = false;
        xEventGroupClearBits(station->event_group_, HOSTED_READY | WIFI_EVENT_CONNECTED);
        xEventGroupSetBits(station->event_group_, HOSTED_FAILED);
        ESP_LOGE(TAG, "C5 transport unavailable; keeping local applications running without host restart");
    }
#endif
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

bool WifiStation::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return Cleanup();
}

bool WifiStation::Cleanup() {
    // Disable callbacks before unregistering them or stopping the driver, so
    // disconnect events during teardown cannot reconnect or schedule scans.
    active_ = false;
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    bool clean = true;
    if (instance_any_id_ != nullptr) {
        if (CheckWifiResult(esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_), "unregister WiFi handler")) {
            instance_any_id_ = nullptr;
        } else {
            clean = false;
        }
    }
    if (instance_got_ip_ != nullptr) {
        if (CheckWifiResult(esp_event_handler_instance_unregister(
                IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_), "unregister IP handler")) {
            instance_got_ip_ = nullptr;
        } else {
            clean = false;
        }
    }
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        if (CheckWifiResult(esp_timer_delete(timer_handle_), "delete scan timer")) {
            timer_handle_ = nullptr;
        } else {
            clean = false;
        }
    }

    // Even a timed-out init may have succeeded on the remote C5. Always try
    // deinit after an attempted init before starting a fresh transaction.
    if (init_attempted_) {
        esp_wifi_scan_stop();
        auto err = esp_wifi_stop();
        if (err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
            CheckWifiResult(err, "stop WiFi");
        }
        err = esp_wifi_deinit();
        if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT) {
            init_attempted_ = false;
        } else {
            CheckWifiResult(err, "deinit WiFi");
            clean = false;
        }
    }

    if (clean && station_netif_ != nullptr) {
        // Also detach the default WiFi driver and its event handlers.
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    connect_queue_.clear();
    ssid_.clear();
    ip_address_.clear();
    reconnect_count_ = 0;
    return clean;
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

bool WifiStation::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_) return true;
    if (!Cleanup()) return false;

    for (int attempt = 1; attempt <= kStartAttempts; ++attempt) {
        const auto err = StartOnce();
        if (err == ESP_OK) return true;
        ESP_LOGW(TAG, "WiFi startup attempt %d/%d failed: %s (0x%x)",
                 attempt, kStartAttempts, esp_err_to_name(err), err);
        if (!Cleanup()) {
            ESP_LOGE(TAG, "WiFi cleanup failed; leaving network unavailable");
            return false;
        }
        if (attempt < kStartAttempts) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "WiFi unavailable after %d attempts; continuing offline", kStartAttempts);
    return false;
}

esp_err_t WifiStation::StartOnce() {
    auto hosted_err = PrepareHostedWifi();
    if (hosted_err != ESP_OK) return hosted_err;
    auto err = esp_netif_init();
    if (err != ESP_OK) return err;
    station_netif_ = esp_netif_create_default_wifi_sta();
    if (station_netif_ == nullptr) return ESP_ERR_NO_MEM;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    init_attempted_ = true;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_);
    if (err != ESP_OK) return err;

    // Create the timer before start: STA_START can immediately trigger scans.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* station = static_cast<WifiStation*>(arg);
            if (station->active_ && !CheckWifiResult(
                    esp_wifi_scan_start(nullptr, false), "scan WiFi")) {
                station->ScheduleScan();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    err = esp_timer_create(&timer_args, &timer_handle_);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    active_ = true;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    if (max_tx_power_ != 0) {
        CheckWifiResult(esp_wifi_set_max_tx_power(max_tx_power_), "set WiFi TX power");
    }
    return ESP_OK;
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    if (!active_) return false;
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::ScheduleScan() {
    if (active_ && timer_handle_ != nullptr) {
        esp_timer_start_once(timer_handle_, kScanDelayUs);
    }
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    if (!CheckWifiResult(esp_wifi_scan_get_ap_num(&ap_num), "read scan count") || ap_num == 0) {
        ScheduleScan();
        return;
    }
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    if (ap_records == nullptr) {
        ESP_LOGW(TAG, "Not enough memory for WiFi scan results");
        ScheduleScan();
        return;
    }
    if (!CheckWifiResult(esp_wifi_scan_get_ap_records(&ap_num, ap_records), "read scan results")) {
        free(ap_records);
        ScheduleScan();
        return;
    }
    connect_queue_.clear();
    // sort by rssi descending
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid,
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Wait for next scan");
        ScheduleScan();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    if (!active_ || connect_queue_.empty()) return;
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    if (!CheckWifiResult(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "configure WiFi")) {
        ScheduleScan();
        return;
    }

    reconnect_count_ = 0;
    if (!CheckWifiResult(esp_wifi_connect(), "connect WiFi")) ScheduleScan();
}

int8_t WifiStation::GetRssi() {
    // Get station info
    wifi_ap_record_t ap_info;
    if (!IsConnected() || !CheckWifiResult(esp_wifi_sta_get_ap_info(&ap_info), "read RSSI")) return -127;
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Get station info
    wifi_ap_record_t ap_info;
    if (!IsConnected() || !CheckWifiResult(esp_wifi_sta_get_ap_info(&ap_info), "read channel")) return 0;
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return active_ && (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED);
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    if (active_) CheckWifiResult(esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE), "set WiFi power save");
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (!this_->active_) return;
    if (event_id == WIFI_EVENT_STA_START) {
        if (!CheckWifiResult(esp_wifi_scan_start(nullptr, false), "start WiFi scan")) this_->ScheduleScan();
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            if (!CheckWifiResult(esp_wifi_connect(), "reconnect WiFi")) this_->ScheduleScan();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }

        ESP_LOGI(TAG, "No more AP to connect, wait for next scan");
        this_->ScheduleScan();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (!this_->active_) return;
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());

    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
}
