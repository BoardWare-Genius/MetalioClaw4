#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct HubDevice {
    std::string id;
    std::string name;
    std::string state;
    std::string model;
    std::string address;
};

struct HubSnapshot {
    enum class Status { Unconfigured, Connecting, Online, Offline, Unauthorized, InvalidResponse };
    enum class ServiceStatus { Unsupported, Online, Starting, Offline, Unauthorized, Unconfigured, Unknown };
    Status status = Status::Unconfigured;
    std::vector<HubDevice> devices;
    ServiceStatus nanomuse = ServiceStatus::Unsupported;
};

// Network work never touches LVGL. The screen reads a bounded snapshot.
class DeviceHubClient {
public:
    static DeviceHubClient& GetInstance();
    bool Start();
    void Pause();
    bool Configure(const std::string& url, const std::string& key);
    std::string GetUrl();
    HubSnapshot Snapshot();
    void Refresh();

private:
    DeviceHubClient();
    static void Run(void* context);
    void Poll();

    std::mutex mutex_;
    TaskHandle_t task_ = nullptr;
    bool active_ = false;
    unsigned generation_ = 0;
    std::string url_;
    std::string key_;
    HubSnapshot snapshot_;
};
