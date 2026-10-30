#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebSocket;

struct NanoMuseChatSnapshot {
    enum class State { Unconfigured, Connecting, Ready, Sending, Running, Approval, Cancelled, Offline, Error };
    enum class Error { None, Connection, Protocol, Timeout, Server, Memory };
    struct Message {
        enum class Role { User, Assistant, System };
        Role role;
        std::string text;
    };
    State state = State::Unconfigured;
    Error error = Error::None;
    bool ready = false;
    uint32_t revision = 0;
    std::vector<Message> messages;
    bool Busy() const { return state == State::Sending || state == State::Running || state == State::Approval; }
};

// Only the worker owns the socket. Callbacks update data, never LVGL objects.
class NanoMuseChatClient {
public:
    static NanoMuseChatClient& GetInstance();
    ~NanoMuseChatClient();
    bool ConfigureKey(const std::string& key);
    bool HasKey();
    bool Start(const std::string& gateway_url);
    void Pause();
    bool Send(const std::string& text);
    void Cancel();
    NanoMuseChatSnapshot Snapshot();

private:
    NanoMuseChatClient();
    static void Run(void* context);
    void Step();
    void Receive(uint32_t generation, const char* data, size_t length, bool binary);
    void Disconnected(uint32_t generation);
    void AddMessage(NanoMuseChatSnapshot::Message::Role role, const std::string& text);

    std::mutex mutex_;
    TaskHandle_t task_ = nullptr;
    bool active_ = false;
    uint32_t generation_ = 0, worker_generation_ = 0;
    std::string url_, key_, pending_;
    NanoMuseChatSnapshot snapshot_;
    uint32_t next_id_ = 0, turn_id_ = 0, cancel_id_ = 0, progress_ = 0;
    int64_t activity_at_ = 0, turn_at_ = 0, retry_at_ = 0, hello_at_ = 0;
    bool identify_ = false, broken_ = false;
    std::unique_ptr<WebSocket> socket_;
};
