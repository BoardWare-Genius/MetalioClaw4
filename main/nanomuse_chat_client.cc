#include "nanomuse_chat_client.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cJSON.h>
#include <esp_timer.h>
#include <web_socket.h>

#include "board.h"
#include "settings.h"

namespace {
using State = NanoMuseChatSnapshot::State;
using Error = NanoMuseChatSnapshot::Error;
using Role = NanoMuseChatSnapshot::Message::Role;
constexpr size_t kMaxWire = 8192;

bool ValidId(const cJSON* id) {
    return cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX &&
           id->valuedouble == static_cast<uint32_t>(id->valuedouble);
}

std::string Wire(cJSON* root) {
    if (!root) return {};
    char* raw = cJSON_PrintUnformatted(root);
    std::string wire = raw ? raw : "";
    cJSON_free(raw);
    cJSON_Delete(root);
    return wire;
}
}  // namespace

NanoMuseChatClient& NanoMuseChatClient::GetInstance() {
    static NanoMuseChatClient instance;
    return instance;
}

NanoMuseChatClient::NanoMuseChatClient() {
    Settings settings("device_hub", false);
    key_ = settings.GetString("chat_key");
}
NanoMuseChatClient::~NanoMuseChatClient() = default;

bool NanoMuseChatClient::ConfigureKey(const std::string& key) {
    if (!key.empty() && (key.size() < 16 || key.size() > 127)) return false;
    for (unsigned char c : key) if (c < 33 || c > 126) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    key_ = key;
    Settings settings("device_hub", true);
    settings.SetString("chat_key", key_);
    return true;
}

bool NanoMuseChatClient::HasKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !key_.empty();
}

bool NanoMuseChatClient::Start(const std::string& gateway_url) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    active_ = true;
    snapshot_ = {};
    snapshot_.revision = generation_;
    pending_.clear();
    turn_id_ = cancel_id_ = 0;
    broken_ = identify_ = false;
    url_.clear();
    if (gateway_url.rfind("http://", 0) == 0) url_ = "ws://" + gateway_url.substr(7);
    else if (gateway_url.rfind("https://", 0) == 0) url_ = "wss://" + gateway_url.substr(8);
    if (!url_.empty()) url_ += "/device";
    if (!url_.empty() && !key_.empty()) snapshot_.state = State::Connecting;
    if (!task_ && xTaskCreate(Run, "nano_chat", 8192, this, 3, &task_) != pdPASS) {
        snapshot_.state = State::Error;
        snapshot_.error = Error::Memory;
        active_ = false;
        return false;
    }
    xTaskNotifyGive(task_);
    return true;
}

void NanoMuseChatClient::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    ++generation_;
    pending_.clear();
    turn_id_ = cancel_id_ = 0;
    snapshot_ = {};
    snapshot_.revision = generation_;
    if (task_) xTaskNotifyGive(task_);
}

void NanoMuseChatClient::AddMessage(Role role, const std::string& text) {
    if (text.empty()) return;
    if (snapshot_.messages.size() >= 12) snapshot_.messages.erase(snapshot_.messages.begin());
    snapshot_.messages.push_back({role, text});
    ++snapshot_.revision;
}

bool NanoMuseChatClient::Send(const std::string& text) {
    if (text.empty() || text.size() > 4000 || text.find('\0') != std::string::npos ||
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); })) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || !snapshot_.ready || snapshot_.Busy() || !pending_.empty() || next_id_ == UINT32_MAX) return false;
    auto* root = cJSON_CreateObject();
    if (!root) return false;
    const uint32_t id = next_id_ + 1;
    cJSON_AddStringToObject(root, "type", "text");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "text", text.c_str());
    // First version displays approvals for processing on the nanoMuse webpage.
    cJSON_AddBoolToObject(root, "approvals", true);
    auto wire = Wire(root);
    if (wire.empty() || wire.size() > kMaxWire) return false;
    pending_ = std::move(wire);
    next_id_ = turn_id_ = id;
    progress_ = 0;
    activity_at_ = turn_at_ = esp_timer_get_time();
    snapshot_.state = State::Sending;
    snapshot_.error = Error::None;
    AddMessage(Role::User, text);
    xTaskNotifyGive(task_);
    return true;
}

void NanoMuseChatClient::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!snapshot_.Busy()) return;
    cancel_id_ = turn_id_;
    turn_id_ = 0;  // Immediately reject late replies, even before cancel reaches the server.
    pending_.clear();
    snapshot_.state = State::Cancelled;
    ++snapshot_.revision;
    if (task_) xTaskNotifyGive(task_);
}

NanoMuseChatSnapshot NanoMuseChatClient::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void NanoMuseChatClient::Disconnected(uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || generation != generation_) return;
    if (snapshot_.ready || snapshot_.Busy()) {
        AddMessage(Role::System, "连接断开，重连后将开始新会话");
    }
    snapshot_.ready = false;
    snapshot_.state = State::Offline;
    snapshot_.error = Error::Connection;
    pending_.clear();
    turn_id_ = cancel_id_ = 0;
    broken_ = true;
    ++snapshot_.revision;
}

void NanoMuseChatClient::Receive(uint32_t generation, const char* data, size_t length, bool binary) {
    if (binary || !data || !length) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || generation != generation_) return;
    cJSON* root = length <= kMaxWire ? cJSON_ParseWithLength(data, length) : nullptr;
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(kind) || !kind->valuestring) {
        snapshot_.state = State::Error;
        snapshot_.error = Error::Protocol;
        snapshot_.ready = false;
        broken_ = true;
        ++snapshot_.revision;
        cJSON_Delete(root);
        return;
    }
    const std::string type = kind->valuestring;
    if (type == "hello") {
        const auto* protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
        if (cJSON_IsNumber(protocol) && protocol->valuedouble == 1) {
            snapshot_.ready = true;
            snapshot_.state = State::Ready;
            snapshot_.error = Error::None;
            identify_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "device_identity"));
        } else {
            snapshot_.state = State::Error;
            snapshot_.error = Error::Protocol;
            snapshot_.ready = false;
            broken_ = true;
        }
        ++snapshot_.revision;
        cJSON_Delete(root);
        return;
    }
    const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!snapshot_.ready || !ValidId(id) || static_cast<uint32_t>(id->valuedouble) != turn_id_) {
        cJSON_Delete(root);
        return;
    }
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, "text");
    const char* text = cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
    if (std::strlen(text) > 6000) {
        snapshot_.state = State::Error;
        snapshot_.error = Error::Protocol;
        cancel_id_ = turn_id_;
        turn_id_ = 0;
    } else if (type == "sent") {
        snapshot_.state = State::Running;
        activity_at_ = esp_timer_get_time();
    } else if (type == "progress") {
        const auto* seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        if (ValidId(seq) && seq->valuedouble > progress_) {
            progress_ = static_cast<uint32_t>(seq->valuedouble);
            activity_at_ = esp_timer_get_time();
        }
    } else if (type == "reply") {
        AddMessage(Role::Assistant, text);
        activity_at_ = esp_timer_get_time();
    } else if (type == "warning" || type == "approval") {
        AddMessage(Role::System, text);
        if (type == "approval") snapshot_.state = State::Approval;
        activity_at_ = esp_timer_get_time();
    } else if (type == "approval_closed") {
        snapshot_.state = State::Running;
        activity_at_ = esp_timer_get_time();
    } else if (type == "done") {
        snapshot_.state = State::Ready;
        turn_id_ = 0;
    } else if (type == "error") {
        AddMessage(Role::System, text);
        snapshot_.state = State::Error;
        snapshot_.error = Error::Server;
        turn_id_ = 0;
    }
    ++snapshot_.revision;
    cJSON_Delete(root);
}

void NanoMuseChatClient::Run(void* context) {
    auto* self = static_cast<NanoMuseChatClient*>(context);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        self->Step();
    }
}

void NanoMuseChatClient::Step() {
    uint32_t generation;
    bool enabled, reset;
    std::string url, key;
    const int64_t now = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool fault = broken_;
        if (fault) ++generation_;  // Fence callbacks from the old socket before destroying it.
        generation = generation_;
        enabled = active_ && !url_.empty() && !key_.empty();
        reset = worker_generation_ != generation || broken_;
        if (reset) {
            worker_generation_ = generation;
            broken_ = identify_ = false;
            retry_at_ = fault ? now + 5000000 : 0;
        }
        url = url_;
        key = key_;
    }
    if (reset || !enabled) socket_.reset();
    if (!enabled) return;
    if (!socket_) {
        if (now < retry_at_) return;
        retry_at_ = now + 5000000;
        auto* network = Board::GetInstance().GetNetwork();
        socket_ = network ? network->CreateWebSocket(3) : nullptr;
        if (!socket_) { Disconnected(generation); return; }
        socket_->SetReceiveBufferSize(kMaxWire + 512);
        const std::string auth = "Bearer " + key;
        socket_->SetHeader("Authorization", auth.c_str());
        socket_->OnData([this, generation](const char* data, size_t length, bool binary) {
            Receive(generation, data, length, binary);
        });
        socket_->OnDisconnected([this, generation]() { Disconnected(generation); });
        socket_->OnError([this, generation](int) { Disconnected(generation); });
        hello_at_ = now;
        if (!socket_->Connect(url.c_str())) { Disconnected(generation); return; }
    }
    std::string wire;
    bool identify = false;
    uint32_t cancel = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || generation != generation_ || broken_) return;
        if (!snapshot_.ready) {
            if (now - hello_at_ > 10000000) {
                snapshot_.state = State::Error;
                snapshot_.error = Error::Protocol;
                broken_ = true;
                ++snapshot_.revision;
            }
            return;
        }
        if (snapshot_.Busy() && (now - activity_at_ > 180000000 || now - turn_at_ > 900000000)) {
            cancel_id_ = turn_id_;
            turn_id_ = 0;
            pending_.clear();
            snapshot_.state = State::Error;
            snapshot_.error = Error::Timeout;
            ++snapshot_.revision;
        }
        identify = identify_;
        identify_ = false;
        cancel = cancel_id_;
        cancel_id_ = 0;
        wire.swap(pending_);
    }
    if (identify && !socket_->Send("{\"type\":\"identify\",\"model\":\"metalio_claw_4\"}")) { Disconnected(generation); return; }
    if (cancel && !socket_->Send("{\"type\":\"cancel\",\"id\":" + std::to_string(cancel) + "}")) { Disconnected(generation); return; }
    if (!wire.empty() && !socket_->Send(wire)) Disconnected(generation);
}
