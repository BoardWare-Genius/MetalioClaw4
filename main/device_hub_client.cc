#include "device_hub_client.h"

#include <cJSON.h>
#include <cstring>
#include <utility>
#include <esp_timer.h>

#include "board.h"
#include "settings.h"

namespace {
constexpr size_t kMaxBody = 4096;

bool BoundedText(const cJSON* value, size_t maximum) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    const size_t length = std::strlen(value->valuestring);
    if (length == 0 || length > maximum) return false;
    for (size_t i = 0; i < length; ++i) {
        if (static_cast<unsigned char>(value->valuestring[i]) < 32) return false;
    }
    return true;
}

bool OptionalText(const cJSON* entry, const char* key, size_t maximum, std::string& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(entry, key);
    if (value == nullptr || cJSON_IsNull(value)) return true;
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    if (value->valuestring[0] && !BoundedText(value, maximum)) return false;
    out = value->valuestring;
    return true;
}

HubSnapshot::ServiceStatus ParseNanoMuse(const cJSON* root) {
    using Status = HubSnapshot::ServiceStatus;
    const auto* services = cJSON_GetObjectItemCaseSensitive(root, "services");
    if (services == nullptr) return Status::Unsupported;
    const auto* nano = cJSON_GetObjectItemCaseSensitive(services, "nanomuse");
    if (!cJSON_IsObject(services) || !cJSON_IsObject(nano)) return Status::Unknown;
    const auto* state = cJSON_GetObjectItemCaseSensitive(nano, "state");
    if (!BoundedText(state, 32)) return Status::Unknown;
    if (std::strcmp(state->valuestring, "online") == 0) return Status::Online;
    if (std::strcmp(state->valuestring, "starting") == 0) return Status::Starting;
    if (std::strcmp(state->valuestring, "offline") == 0) return Status::Offline;
    if (std::strcmp(state->valuestring, "auth_error") == 0) return Status::Unauthorized;
    if (std::strcmp(state->valuestring, "not_configured") == 0) return Status::Unconfigured;
    return Status::Unknown;
}

bool Parse(const std::string& body, HubSnapshot& snapshot) {
    auto& devices = snapshot.devices;
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size() + 1);
    if (root == nullptr) return false;
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON* entries = cJSON_GetObjectItemCaseSensitive(root, "devices");
    bool valid = cJSON_IsNumber(version) && version->valuedouble == 1 &&
                 cJSON_IsArray(entries) && cJSON_GetArraySize(entries) <= 4;
    if (valid) {
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, entries) {
            const cJSON* id = cJSON_GetObjectItemCaseSensitive(entry, "id");
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            const cJSON* state = cJSON_GetObjectItemCaseSensitive(entry, "state");
            const cJSON* online = cJSON_GetObjectItemCaseSensitive(entry, "online");
            if (!BoundedText(id, 32) || !BoundedText(name, 48) ||
                !BoundedText(state, 16) || !cJSON_IsTrue(online)) {
                valid = false;
                break;
            }
            std::string model, address;
            if (!OptionalText(entry, "model", 48, model) ||
                !OptionalText(entry, "address", 64, address)) {
                valid = false;
                break;
            }
            devices.push_back({id->valuestring, name->valuestring, state->valuestring, model, address});
        }
    }
    if (valid) snapshot.nanomuse = ParseNanoMuse(root);
    cJSON_Delete(root);
    if (!valid) devices.clear();
    return valid;
}
}  // namespace

DeviceHubClient& DeviceHubClient::GetInstance() {
    static DeviceHubClient instance;
    return instance;
}

DeviceHubClient::DeviceHubClient() {
    Settings settings("device_hub", false);
    url_ = settings.GetString("url");
    key_ = settings.GetString("key");
    if (!url_.empty() && !key_.empty()) snapshot_.status = HubSnapshot::Status::Connecting;
}

bool DeviceHubClient::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
    if (task_ == nullptr && xTaskCreate(Run, "device_hub", 6144, this, 3, &task_) != pdPASS) {
        active_ = false;
        snapshot_ = {};
        if (!url_.empty()) snapshot_.status = HubSnapshot::Status::Offline;
        return false;
    }
    xTaskNotifyGive(task_);
    return true;
}

void DeviceHubClient::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    ++generation_;
    snapshot_ = {};
    if (!url_.empty()) snapshot_.status = HubSnapshot::Status::Connecting;
}

bool DeviceHubClient::Configure(const std::string& url, const std::string& key) {
    if (!url.empty()) {
        if (url.size() > 192 || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) ||
            key.size() < 16 || key.size() > 127) return false;
        const auto start = url.find("://") + 3;
        if (start == url.size() || url[start] == '/' ||
            url.find_first_of("@?#\\", start) != std::string::npos) return false;
        for (unsigned char c : url) if (c < 33 || c > 126) return false;
        for (unsigned char c : key) if (c < 33 || c > 126) return false;
    }
    std::string normalized = url;
    while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
    std::lock_guard<std::mutex> lock(mutex_);
    url_ = normalized;
    key_ = url.empty() ? "" : key;
    ++generation_;
    snapshot_ = {};
    if (!url_.empty()) snapshot_.status = HubSnapshot::Status::Connecting;
    Settings settings("device_hub", true);
    settings.SetString("url", url_);
    settings.SetString("key", key_);
    if (task_ != nullptr) xTaskNotifyGive(task_);
    return true;
}

std::string DeviceHubClient::GetUrl() {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

HubSnapshot DeviceHubClient::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void DeviceHubClient::Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ != nullptr) xTaskNotifyGive(task_);
}

void DeviceHubClient::Run(void* context) {
    auto* self = static_cast<DeviceHubClient*>(context);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        self->Poll();
    }
}

void DeviceHubClient::Poll() {
    std::string url, key;
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || url_.empty() || key_.empty()) return;
        url = url_ + "/console/devices";
        key = key_;
        generation = generation_;
    }
    HubSnapshot next;
    next.status = HubSnapshot::Status::Offline;
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network != nullptr ? network->CreateHttp(2) : nullptr;
    if (http != nullptr) {
        http->SetTimeout(3000);
        http->SetHeader("Authorization", "Bearer " + key);
        http->SetHeader("Accept", "application/json");
        http->SetHeader("Connection", "close");
        if (http->Open("GET", url)) {
            const int status = http->GetStatusCode();
            if (status == 401 || status == 403) {
                next.status = HubSnapshot::Status::Unauthorized;
            } else if (status == 200) {
                std::string body;
                char buffer[512];
                bool complete = false;
                const int64_t deadline = esp_timer_get_time() + 5000000;
                while (body.size() <= kMaxBody) {
                    if (esp_timer_get_time() > deadline) break;
                    const int count = http->Read(buffer, sizeof(buffer));
                    if (count == 0) { complete = true; break; }
                    if (count < 0) break;
                    body.append(buffer, count);
                }
                next.status = complete && body.size() <= kMaxBody && Parse(body, next)
                    ? HubSnapshot::Status::Online : HubSnapshot::Status::InvalidResponse;
            }
        }
        http->Close();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && generation == generation_) snapshot_ = std::move(next);
}
