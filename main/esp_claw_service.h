#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

class EspClawService {
public:
    using ResponseCallback = std::function<void(const std::string&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ProgressCallback = std::function<void(const std::string&)>;

    EspClawService() = default;
    ~EspClawService();

    EspClawService(const EspClawService&) = delete;
    EspClawService& operator=(const EspClawService&) = delete;

    void Initialize(ResponseCallback on_response, ErrorCallback on_error,
                    ProgressCallback on_progress);
    bool Start();
    void Stop();
    bool Submit(const std::string& text, const std::string& session_id = "");
    bool CancelActiveRequest();

    bool Configure(const std::string& api_key,
                   const std::string& base_url,
                   const std::string& model,
                   const std::string& backend);
    bool IsConfigured() const { return configured_.load(); }
    bool IsRunning() const { return running_.load(); }
    std::string GetConfigSummary() const;

    // Called by the ESP-Claw capability bridge while the agent task executes a
    // tool. The callback only reports progress; it does not touch LVGL directly.
    void ReportToolStarted(const char* tool_name);
    void ReportToolFinished(const char* tool_name, int result);
    void ScheduleConversationReset();

private:
    void LoadConfig();
    void NotifyError(const std::string& message) const;
    void NotifyProgress(const std::string& message);
    void MaybeNotifyProgress();
    static void ResponseTaskEntry(void* arg);
    void ResponseTask();
    void ApplyPendingConversationReset();

    void* core_ = nullptr;
    std::atomic<void*> response_task_{nullptr};
    std::atomic<bool> configured_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> request_inflight_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<uint32_t> next_request_id_{1};
    std::atomic<uint32_t> request_start_tick_{0};
    std::atomic<uint32_t> last_progress_tick_{0};
    std::atomic<int> last_progress_phase_{-1};
    std::atomic<uint32_t> tool_step_{0};
    std::atomic<bool> reset_after_request_{false};
    std::string api_key_;
    std::string base_url_;
    std::string model_;
    std::string backend_;
    ResponseCallback on_response_;
    ErrorCallback on_error_;
    ProgressCallback on_progress_;
};
