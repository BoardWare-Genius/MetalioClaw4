#include "control_center_screen.h"

#include <cstdio>
#include <cstring>
#include <font_awesome.h>

#include "board.h"
#include "device_hub_client.h"
#include "nanomuse_chat_client.h"
#include "camera_screen/camera_screen.h"
#include "gps_screen/gps_screen.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "pwr_key_handler.h"
#include "settings_screen/settings_common.h"
#include "settings_screen/settings_screen.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

namespace {
constexpr uint32_t kBg = 0xFFFFFF;
constexpr uint32_t kCard = 0xF3F4F5;
constexpr uint32_t kText = 0x20242A;
constexpr uint32_t kMuted = 0x737B86;
constexpr uint32_t kBorder = 0xE2E5E9;
constexpr uint32_t kAccent = 0x276749;
constexpr uint32_t kOnline = 0x24A15A;
constexpr uint32_t kOffline = 0xA5ABB3;
constexpr uint32_t kError = 0xB54737;

struct ChatUi;
struct Ui {
    lv_obj_t* screen = nullptr;
    lv_obj_t* battery = nullptr;
    lv_obj_t* hub_status = nullptr;
    lv_obj_t* device_summary = nullptr;
    lv_obj_t* device_name = nullptr;
    lv_obj_t* device_role = nullptr;
    lv_obj_t* device_dot = nullptr;
    lv_obj_t* device_count = nullptr;
    lv_obj_t* nanomuse_status = nullptr;
    lv_obj_t* nanomuse_dot = nullptr;
    lv_obj_t* modal = nullptr;
    lv_obj_t* devices = nullptr;
    ChatUi* chat = nullptr;
    lv_timer_t* timer = nullptr;
    std::string last_devices;
};
void RefreshChat(Ui* ui);

lv_obj_t* Label(lv_obj_t* parent, const char* text, int width, uint32_t color = kText) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

lv_obj_t* Icon(lv_obj_t* parent, const char* symbol, uint32_t color = kText) {
    auto* label = Label(parent, symbol, 30, color);
    lv_obj_set_style_text_font(label, &font_awesome_20_4, 0);
    return label;
}

lv_obj_t* Card(lv_obj_t* parent, int height) {
    auto* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, LV_PCT(100), height);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t* Button(lv_obj_t* parent, const char* text, int width, lv_event_cb_t callback, void* data) {
    auto* button = lv_button_create(parent);
    lv_obj_set_size(button, width, 48);
    lv_obj_set_style_bg_color(button, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCard), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(kBorder), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, 12, 0);
    auto* label = Label(button, text, width - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    screen_swipe_back_ignore(button);
    return button;
}

lv_obj_t* IconButton(lv_obj_t* parent, const char* symbol, lv_event_cb_t callback, void* data) {
    auto* button = Button(parent, symbol, 48, callback, data);
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0), &font_awesome_20_4, 0);
    return button;
}

void Back() {
    if (auto* indev = lv_indev_active()) lv_indev_wait_release(indev);
    auto* old = lv_screen_active();
    lv_screen_load(HomeScreen::Create());
    lv_obj_delete_async(old);
}

void LoadApp(lv_obj_t* app, screen_lifecycle_cb_t callback) {
    if (!app) return;
    if (auto* indev = lv_indev_active()) lv_indev_wait_release(indev);
    auto* old = lv_screen_active();
    screen_attach_lifecycle(app, callback);
    lv_screen_load(app);
    lv_obj_delete_async(old);
}

void OpenCamera(lv_event_t*) {
    LoadApp(CameraScreen::Create(), [](screen_lifecycle_event_t event) {
        PwrKey_OnScreenLifecycle("camera", event);
        CameraScreen::LifecycleCallback(event);
    });
}

const char* StateText(const std::string& state) {
    if (state == "busy") return I18n::T("执行中");
    if (state == "recording") return I18n::T("录音中");
    if (state == "approval") return I18n::T("等待审批");
    if (state == "question") return I18n::T("等待回答");
    if (state == "idle") return I18n::T("空闲");
    return I18n::T("未知");
}

const char* StatusText(HubSnapshot::Status status) {
    using Status = HubSnapshot::Status;
    switch (status) {
        case Status::Unconfigured: return I18n::T("尚未配置网关");
        case Status::Connecting: return I18n::T("正在连接网关");
        case Status::Online: return I18n::T("网关已连接");
        case Status::Unauthorized: return I18n::T("网关密钥不正确");
        case Status::InvalidResponse: return I18n::T("网关返回格式不兼容");
        case Status::Offline: return I18n::T("网关未连接");
    }
    return I18n::T("网关未连接");
}

void RefreshNanoMuse(Ui* ui, const HubSnapshot& snapshot) {
    const char* text = StatusText(snapshot.status);
    uint32_t color = kOffline;
    if (snapshot.status == HubSnapshot::Status::Online) {
        using Status = HubSnapshot::ServiceStatus;
        switch (snapshot.nanomuse) {
            case Status::Online: text = I18n::T("服务在线"); color = kOnline; break;
            case Status::Starting: text = I18n::T("服务启动中"); color = 0xB7791F; break;
            case Status::Offline: text = I18n::T("服务离线"); break;
            case Status::Unauthorized: text = I18n::T("nanoMuse 认证失败"); color = kError; break;
            case Status::Unconfigured: text = I18n::T("网关未配置 token"); color = kError; break;
            case Status::Unsupported: text = I18n::T("网关需更新"); break;
            case Status::Unknown: text = I18n::T("服务状态不兼容"); color = kError; break;
        }
    } else if (snapshot.status == HubSnapshot::Status::Unauthorized ||
               snapshot.status == HubSnapshot::Status::InvalidResponse) {
        color = kError;
    }
    lv_label_set_text(ui->nanomuse_status, text);
    lv_obj_set_style_text_color(ui->nanomuse_status, lv_color_hex(color == kOffline ? kMuted : color), 0);
    lv_obj_set_style_bg_color(ui->nanomuse_dot, lv_color_hex(color), 0);
}

void Refresh(Ui* ui) {
    RefreshChat(ui);
    int level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d%%  ·  %s", level,
                      charging ? I18n::T("充电中") : I18n::T("电池供电"));
        lv_label_set_text(ui->battery, text);
    } else {
        lv_label_set_text(ui->battery, I18n::T("电量暂不可用"));
    }
    const auto snapshot = DeviceHubClient::GetInstance().Snapshot();
    RefreshNanoMuse(ui, snapshot);
    using Status = HubSnapshot::Status;
    const bool online = snapshot.status == Status::Online;
    const auto count = online ? snapshot.devices.size() : 0;
    lv_label_set_text(ui->hub_status, StatusText(snapshot.status));
    const bool error = snapshot.status == Status::Unauthorized || snapshot.status == Status::InvalidResponse;
    lv_obj_set_style_text_color(ui->hub_status,
        lv_color_hex(online ? kAccent : (error ? kError : kMuted)), 0);
    lv_obj_set_style_bg_color(ui->device_dot, lv_color_hex(count ? kOnline : kOffline), 0);
    char summary[128];
    std::snprintf(summary, sizeof(summary), I18n::T("%d 台设备在线"), static_cast<int>(count));
    lv_label_set_text(ui->device_count, summary);
    lv_label_set_text(ui->device_name, "Muse Gadget");
    lv_label_set_text(ui->device_role, I18n::T("对话设备"));
    if (count == 1) {
        const auto& device = snapshot.devices[0];
        lv_label_set_text(ui->device_name, device.name.c_str());
        std::string role = device.model.empty() ? std::string(I18n::T("型号未上报")) + " · " : "";
        role += StateText(device.state);
        lv_label_set_text(ui->device_role, role.c_str());
        std::snprintf(summary, sizeof(summary), "%s %s", device.address.empty() ? I18n::T("会话") : "IP",
                      device.address.empty() ? device.id.c_str() : device.address.c_str());
    } else if (count > 1) {
        lv_label_set_text(ui->device_name, I18n::T("设备会话"));
        lv_label_set_text(ui->device_role, I18n::T("点击查看设备列表"));
        std::snprintf(summary, sizeof(summary), I18n::T("%d 台在线 · 点击查看"), static_cast<int>(count));
    } else {
        std::snprintf(summary, sizeof(summary), "%s", online ? I18n::T("暂无设备连接网关") : I18n::T("连接网关后查看"));
    }
    lv_label_set_text(ui->device_summary, summary);
    if (!ui->devices) return;

    // Only rebuild the inventory when it changes; never retain stale online entries.
    std::string signature = std::to_string(static_cast<int>(snapshot.status));
    for (const auto& device : snapshot.devices) {
        signature += "|" + device.id + "|" + device.name + "|" + device.state + "|" + device.model + "|" + device.address;
    }
    if (signature == ui->last_devices) return;
    ui->last_devices = signature;
    lv_obj_clean(ui->devices);
    if (!online || count == 0) {
        Label(ui->devices, online ? I18n::T("暂无设备连接网关") : StatusText(snapshot.status), 600, kMuted);
        return;
    }
    for (const auto& device : snapshot.devices) {
        auto* card = Card(ui->devices, 120);
        auto* name = Label(card, device.name.c_str(), 430);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        auto* state = Label(card, StateText(device.state), 160, kAccent);
        lv_obj_align(state, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, 0);
        const std::string info = (device.model.empty() ? I18n::T("型号未上报") : device.model) +
            (device.address.empty() ? std::string() : " · IP " + device.address);
        auto* details = Label(card, info.c_str(), 620, kMuted);
        lv_label_set_long_mode(details, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(details, 0, 30);
        const std::string session = std::string(I18n::T("会话")) + " " + device.id;
        auto* id = Label(card, session.c_str(), 620, kMuted);
        lv_obj_set_pos(id, 0, 60);
    }
}

void CloseModal(Ui* ui) {
    if (ui->chat) NanoMuseChatClient::GetInstance().Pause();
    ui->chat = nullptr;
    auto* modal = ui->modal;
    ui->modal = nullptr;
    ui->devices = nullptr;
    ui->last_devices.clear();
    if (modal) lv_obj_delete(modal);
}

lv_obj_t* Overlay(Ui* ui, const char* title, const char* hint) {
    CloseModal(ui);
    auto* overlay = lv_obj_create(ui->screen);
    ui->modal = overlay;
    screen_strip_obj_chrome(overlay);
    lv_obj_set_size(overlay, 720, 720);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(overlay);
    auto* heading = Label(overlay, title, 580);
    lv_obj_set_style_text_font(heading, &font_puhui_30_4, 0);
    lv_obj_set_pos(heading, 28, 26);
    auto* subtitle = Label(overlay, hint, 664, kMuted);
    lv_obj_set_pos(subtitle, 28, 78);
    auto* close = IconButton(overlay, FONT_AWESOME_XMARK, [](lv_event_t* event) {
        CloseModal(static_cast<Ui*>(lv_event_get_user_data(event)));
    }, ui);
    lv_obj_set_pos(close, 644, 24);
    return overlay;
}

struct ConfigUi {
    Ui* ui;
    lv_obj_t* overlay;
    lv_obj_t* url;
    lv_obj_t* key;
    lv_obj_t* keyboard;
    lv_obj_t* error;
};

void Configure(lv_event_t* event) {
    auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
    auto* config = new ConfigUi{};
    config->ui = ui;
    auto* overlay = Overlay(ui, I18n::T("连接设备网关"), I18n::T("填写网关地址与控制台密钥"));
    config->overlay = overlay;
    lv_obj_add_event_cb(overlay, [](lv_event_t* e) {
        delete static_cast<ConfigUi*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, config);
    config->url = lv_textarea_create(overlay);
    lv_obj_set_pos(config->url, 26, 114);
    lv_obj_set_size(config->url, 668, 58);
    lv_textarea_set_one_line(config->url, true);
    lv_textarea_set_max_length(config->url, 192);
    lv_textarea_set_placeholder_text(config->url, "http://192.168.1.10:8765");
    lv_textarea_set_text(config->url, DeviceHubClient::GetInstance().GetUrl().c_str());
    config->key = lv_textarea_create(overlay);
    lv_obj_set_pos(config->key, 26, 188);
    lv_obj_set_size(config->key, 668, 58);
    lv_textarea_set_one_line(config->key, true);
    lv_textarea_set_max_length(config->key, 127);
    lv_textarea_set_password_mode(config->key, true);
    lv_textarea_set_placeholder_text(config->key, "METALIO_CONSOLE_KEY");
    for (auto* field : {config->url, config->key}) {
        lv_obj_set_style_bg_color(field, lv_color_hex(kCard), 0);
        lv_obj_set_style_text_color(field, lv_color_hex(kText), 0);
        lv_obj_set_style_text_font(field, &font_puhui_20_4, 0);
        lv_obj_set_style_border_color(field, lv_color_hex(kBorder), 0);
        lv_obj_set_style_radius(field, 12, 0);
    }
    config->error = Label(overlay, "", 668, kError);
    lv_obj_set_pos(config->error, 26, 264);
    config->keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(config->keyboard, 720, 270);
    lv_obj_align(config->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(config->keyboard, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_color(config->keyboard, lv_color_hex(kBg), LV_PART_ITEMS);
    lv_obj_set_style_text_color(config->keyboard, lv_color_hex(kText), LV_PART_ITEMS);
    lv_keyboard_set_mode(config->keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(config->keyboard, config->url);
    for (auto* field : {config->url, config->key}) {
        lv_obj_add_event_cb(field, [](lv_event_t* e) {
            auto* state = static_cast<ConfigUi*>(lv_event_get_user_data(e));
            lv_keyboard_set_textarea(state->keyboard, static_cast<lv_obj_t*>(lv_event_get_target(e)));
        }, LV_EVENT_FOCUSED, config);
    }
    auto* save = Button(overlay, I18n::T("保存并连接"), 204, [](lv_event_t* e) {
        auto* state = static_cast<ConfigUi*>(lv_event_get_user_data(e));
        if (!DeviceHubClient::GetInstance().Configure(lv_textarea_get_text(state->url),
                                                     lv_textarea_get_text(state->key))) {
            lv_label_set_text(state->error, I18n::T("请输入有效地址和16至127位密钥"));
            return;
        }
        CloseModal(state->ui);
    }, config);
    lv_obj_set_pos(save, 26, 366);
    auto* cancel = Button(overlay, I18n::T("取消"), 144, [](lv_event_t* e) {
        auto* state = static_cast<ConfigUi*>(lv_event_get_user_data(e));
        CloseModal(state->ui);
    }, config);
    lv_obj_set_pos(cancel, 246, 366);
}

struct ChatUi {
    Ui* ui;
    lv_obj_t* list = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* input = nullptr;
    lv_obj_t* keyboard = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* send = nullptr;
    lv_obj_t* cancel = nullptr;
    std::string last_messages;
};

void ChatKeyboard(ChatUi* chat, bool visible) {
    if (visible) lv_obj_remove_flag(chat->keyboard, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(chat->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(chat->list, visible ? 180 : 382);
    lv_obj_set_pos(chat->input, 28, visible ? 318 : 520);
    lv_obj_set_pos(chat->toolbar, 28, visible ? 396 : 598);
}

const char* ChatStatus(const NanoMuseChatSnapshot& snapshot) {
    using State = NanoMuseChatSnapshot::State;
    using Error = NanoMuseChatSnapshot::Error;
    switch (snapshot.state) {
        case State::Unconfigured: return I18n::T("请配置网关地址与对话密钥");
        case State::Connecting: return I18n::T("正在连接对话服务");
        case State::Ready: return I18n::T("已连接，可以发送消息");
        case State::Sending: return I18n::T("正在发送");
        case State::Running: return I18n::T("正在执行，等待回复");
        case State::Approval: return I18n::T("等待确认，请在 nanoMuse 网页处理");
        case State::Cancelled: return I18n::T("已取消，可以继续发送");
        case State::Offline: return I18n::T("对话断开，检查网络或设备密钥");
        case State::Error:
            if (snapshot.error == Error::Timeout) return I18n::T("对话超时，请重试");
            if (snapshot.error == Error::Protocol) return I18n::T("对话协议不兼容");
            if (snapshot.error == Error::Memory) return I18n::T("内存不足");
            return I18n::T("对话出错，请查看提示");
    }
    return I18n::T("未知");
}

void RefreshChat(Ui* ui) {
    auto* chat = ui->chat;
    if (!chat) return;
    const auto snapshot = NanoMuseChatClient::GetInstance().Snapshot();
    lv_label_set_text(chat->status, ChatStatus(snapshot));
    if (snapshot.ready && !snapshot.Busy()) lv_obj_remove_state(chat->send, LV_STATE_DISABLED);
    else lv_obj_add_state(chat->send, LV_STATE_DISABLED);
    if (snapshot.Busy()) lv_obj_remove_state(chat->cancel, LV_STATE_DISABLED);
    else lv_obj_add_state(chat->cancel, LV_STATE_DISABLED);
    std::string signature;
    for (const auto& message : snapshot.messages) {
        signature += std::to_string(static_cast<int>(message.role)) + ":" + std::to_string(message.text.size()) + ":" + message.text;
    }
    if (signature == chat->last_messages) return;
    chat->last_messages = std::move(signature);
    lv_obj_clean(chat->list);
    using Role = NanoMuseChatSnapshot::Message::Role;
    for (const auto& message : snapshot.messages) {
        auto* card = Card(chat->list, 0);
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        Label(card, message.role == Role::User ? I18n::T("你") : (message.role == Role::Assistant ? "nanoMuse" : I18n::T("提示")),
              600, message.role == Role::System ? kError : kAccent);
        Label(card, message.role == Role::System ? I18n::T(message.text.c_str()) : message.text.c_str(), 600);
    }
    if (auto* last = lv_obj_get_child(chat->list, -1)) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
}

void OpenChat(Ui* ui);
void ChatKey(Ui* ui) {
    auto* overlay = Overlay(ui, I18n::T("对话密钥"), I18n::T("填写 Gateway 的设备密钥，用于发送对话"));
    auto* input = lv_textarea_create(overlay);
    lv_obj_set_pos(input, 28, 132);
    lv_obj_set_size(input, 664, 64);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_password_mode(input, true);
    lv_textarea_set_max_length(input, 127);
    lv_textarea_set_placeholder_text(input, "METALIO_DEVICE_KEY");
    lv_obj_set_style_text_font(input, &font_puhui_20_4, 0);
    auto* error = Label(overlay, "", 664, kError);
    lv_obj_set_pos(error, 28, 218);
    auto* keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(keyboard, 720, 270);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, input);
    struct KeyUi { Ui* ui; lv_obj_t* input; lv_obj_t* error; };
    auto* state = new KeyUi{ui, input, error};
    lv_obj_add_event_cb(overlay, [](lv_event_t* e) { delete static_cast<KeyUi*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, state);
    auto* save = Button(overlay, I18n::T("保存并连接"), 204, [](lv_event_t* e) {
        auto* state = static_cast<KeyUi*>(lv_event_get_user_data(e));
        if (!NanoMuseChatClient::GetInstance().ConfigureKey(lv_textarea_get_text(state->input))) {
            lv_label_set_text(state->error, I18n::T("请输入16至127位设备密钥"));
            return;
        }
        auto* ui = state->ui;
        if (NanoMuseChatClient::GetInstance().HasKey()) OpenChat(ui);
        else CloseModal(ui);
    }, state);
    lv_obj_set_pos(save, 28, 310);
    auto* cancel = Button(overlay, I18n::T("取消"), 144, [](lv_event_t* e) {
        CloseModal(static_cast<Ui*>(lv_event_get_user_data(e)));
    }, ui);
    lv_obj_set_pos(cancel, 248, 310);
}

void OpenChat(Ui* ui) {
    auto* overlay = Overlay(ui, I18n::T("nanoMuse 对话"), I18n::T("关闭页面结束会话；取消不撤销已执行的操作"));
    auto* chat = new ChatUi{};
    chat->ui = ui;
    ui->chat = chat;
    lv_obj_add_event_cb(overlay, [](lv_event_t* e) { delete static_cast<ChatUi*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, chat);
    chat->status = Label(overlay, "", 664, kAccent);
    lv_obj_set_pos(chat->status, 28, 100);
    lv_label_set_long_mode(chat->status, LV_LABEL_LONG_DOT);
    chat->list = lv_obj_create(overlay);
    screen_strip_obj_chrome(chat->list);
    lv_obj_set_pos(chat->list, 28, 134);
    lv_obj_set_width(chat->list, 664);
    lv_obj_set_flex_flow(chat->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(chat->list, 12, 0);
    lv_obj_add_flag(chat->list, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(chat->list);
    chat->input = lv_textarea_create(overlay);
    lv_obj_set_size(chat->input, 664, 68);
    lv_obj_set_style_text_font(chat->input, &font_puhui_20_4, 0);
    lv_textarea_set_max_length(chat->input, 1000);
    lv_textarea_set_placeholder_text(chat->input, I18n::T("输入消息"));
    chat->keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(chat->keyboard, 720, 270);
    lv_obj_align(chat->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(chat->keyboard, chat->input);
    lv_obj_add_event_cb(chat->input, [](lv_event_t* e) { ChatKeyboard(static_cast<ChatUi*>(lv_event_get_user_data(e)), true); }, LV_EVENT_FOCUSED, chat);
    lv_obj_add_event_cb(chat->keyboard, [](lv_event_t* e) { ChatKeyboard(static_cast<ChatUi*>(lv_event_get_user_data(e)), false); }, LV_EVENT_READY, chat);
    lv_obj_add_event_cb(chat->keyboard, [](lv_event_t* e) { ChatKeyboard(static_cast<ChatUi*>(lv_event_get_user_data(e)), false); }, LV_EVENT_CANCEL, chat);
    chat->toolbar = lv_obj_create(overlay);
    screen_strip_obj_chrome(chat->toolbar);
    lv_obj_set_size(chat->toolbar, 664, 48);
    chat->send = Button(chat->toolbar, I18n::T("发送"), 88, [](lv_event_t* e) {
        auto* chat = static_cast<ChatUi*>(lv_event_get_user_data(e));
        if (NanoMuseChatClient::GetInstance().Send(lv_textarea_get_text(chat->input))) {
            lv_textarea_set_text(chat->input, "");
            ChatKeyboard(chat, false);
            RefreshChat(chat->ui);
        } else lv_label_set_text(chat->status, I18n::T("尚未连接、正在执行或消息过长"));
    }, chat);
    chat->cancel = Button(chat->toolbar, I18n::T("取消"), 96, [](lv_event_t* e) {
        auto* chat = static_cast<ChatUi*>(lv_event_get_user_data(e));
        NanoMuseChatClient::GetInstance().Cancel();
        RefreshChat(chat->ui);
    }, chat);
    lv_obj_set_pos(chat->cancel, 92, 0);
    auto* hello = Button(chat->toolbar, I18n::T("你好"), 88, [](lv_event_t* e) {
        auto* chat = static_cast<ChatUi*>(lv_event_get_user_data(e));
        lv_textarea_set_text(chat->input, I18n::T("你好，请介绍一下你自己。"));
    }, chat);
    lv_obj_set_pos(hello, 192, 0);
    auto* follow = Button(chat->toolbar, I18n::T("继续"), 112, [](lv_event_t* e) {
        lv_textarea_set_text(static_cast<ChatUi*>(lv_event_get_user_data(e))->input, I18n::T("请继续。"));
    }, chat);
    lv_obj_set_pos(follow, 284, 0);
    auto* key = Button(chat->toolbar, I18n::T("对话密钥"), 144, [](lv_event_t* e) {
        ChatKey(static_cast<ChatUi*>(lv_event_get_user_data(e))->ui);
    }, chat);
    lv_obj_set_pos(key, 400, 0);
    auto* keyboard = Button(chat->toolbar, I18n::T("键盘"), 112, [](lv_event_t* e) {
        auto* chat = static_cast<ChatUi*>(lv_event_get_user_data(e));
        ChatKeyboard(chat, lv_obj_has_flag(chat->keyboard, LV_OBJ_FLAG_HIDDEN));
    }, chat);
    lv_obj_set_pos(keyboard, 548, 0);
    ChatKeyboard(chat, false);
    NanoMuseChatClient::GetInstance().Start(DeviceHubClient::GetInstance().GetUrl());
    RefreshChat(ui);
}

void Chat(lv_event_t* event) {
    auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
    if (DeviceHubClient::GetInstance().GetUrl().empty()) { Configure(event); return; }
    if (!NanoMuseChatClient::GetInstance().HasKey()) ChatKey(ui);
    else OpenChat(ui);
}

void AddControl(lv_obj_t* parent, const char* title, int initial, int minimum, bool brightness) {
    auto* panel = Card(parent, 140);
    Label(panel, title, 400);
    auto* value = Label(panel, "", 90, kAccent);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, 0, 0);
    UpdatePctLabel(value, initial);
    lv_obj_t* slider = nullptr;
    CreateSliderRow(panel, minimum, 100, initial, [](lv_event_t*) {}, &slider);
    // The existing helper uses a layout container; place it below the title.
    lv_obj_set_pos(lv_obj_get_parent(slider), 0, 48);
    lv_obj_set_width(lv_obj_get_parent(slider), 620);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, [](lv_event_t* e) {
        UpdatePctLabel(static_cast<lv_obj_t*>(lv_event_get_user_data(e)),
                       lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e))));
    }, LV_EVENT_VALUE_CHANGED, value);
    lv_obj_add_event_cb(slider, brightness ? [](lv_event_t* e) {
        ApplyBrightness(lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e))));
    } : +[](lv_event_t* e) {
        ApplyVolume(lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e))));
    }, LV_EVENT_RELEASED, nullptr);
    screen_swipe_back_ignore(slider);
}
void LocalTools(lv_event_t* event) {
    auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
    auto* overlay = Overlay(ui, I18n::T("本机工具"), I18n::T("亮度、音量与常用工具"));
    auto* body = lv_obj_create(overlay);
    screen_strip_obj_chrome(body);
    lv_obj_set_pos(body, 28, 132);
    lv_obj_set_size(body, 664, 550);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 20, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    AddControl(body, I18n::T("屏幕亮度"), ReadInitialBrightness(), 5, true);
    AddControl(body, I18n::T("扬声器音量"), ReadInitialVolume(), 0, false);
    auto* tools = lv_obj_create(body);
    screen_strip_obj_chrome(tools);
    lv_obj_set_size(tools, 664, 64);
    lv_obj_set_style_bg_opa(tools, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(tools, LV_OBJ_FLAG_SCROLLABLE);
    Button(tools, I18n::T("相机"), 212, OpenCamera, nullptr);
    auto* gps = Button(tools, "GPS", 212, [](lv_event_t*) {
        LoadApp(GpsScreen::Create(), [](screen_lifecycle_event_t event) {
            PwrKey_OnScreenLifecycle("gps", event);
            GpsScreen::LifecycleCallback(event);
        });
    }, nullptr);
    lv_obj_set_pos(gps, 226, 0);
    auto* settings = Button(tools, I18n::T("设置"), 212, [](lv_event_t*) {
        LoadApp(SettingsScreen::Create(), [](screen_lifecycle_event_t event) {
            PwrKey_OnScreenLifecycle("settings", event);
            SettingsScreen::LifecycleCallback(event);
        });
    }, nullptr);
    lv_obj_set_pos(settings, 452, 0);
    Label(body, I18n::T("本机操作无需 nanoMuse，其他设备需连接网关"), 650, kMuted);
}

void Devices(lv_event_t* event) {
    auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
    auto* overlay = Overlay(ui, I18n::T("连接设备"), I18n::T("仅显示已连接网关的真实设备"));
    auto* list = lv_obj_create(overlay);
    ui->devices = list;
    screen_strip_obj_chrome(list);
    lv_obj_set_pos(list, 28, 140);
    lv_obj_set_size(list, 664, 460);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    auto* configure = Button(overlay, I18n::T("连接设置"), 220, Configure, ui);
    lv_obj_set_pos(configure, 28, 638);
    Refresh(ui);
}

lv_obj_t* DeviceCard(Ui* ui, int x, int y, const char* icon, const char* title,
                     const char* role, const char* status, lv_event_cb_t callback,
                     lv_obj_t** status_out = nullptr, lv_obj_t** dot_out = nullptr,
                     lv_obj_t** name_out = nullptr, lv_obj_t** role_out = nullptr) {
    auto* card = Card(ui->screen, 164);
    lv_obj_set_size(card, 326, 164);
    lv_obj_set_pos(card, x, y);
    auto* glyph = Icon(card, icon);
    lv_obj_set_pos(glyph, 0, 0);
    auto* dot = lv_obj_create(card);
    screen_strip_obj_chrome(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 11, 11);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kOffline), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 5);
    auto* name = Label(card, title, 286);
    lv_obj_set_style_text_font(name, &font_puhui_30_4, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, 0, 36);
    auto* kind = Label(card, role, 286, kMuted);
    lv_label_set_long_mode(kind, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(kind, 0, 78);
    auto* info = Label(card, status, 286, kMuted);
    lv_label_set_long_mode(info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(info, 0, 108);
    if (name_out) *name_out = name;
    if (role_out) *role_out = kind;
    if (status_out) *status_out = info;
    if (dot_out) *dot_out = dot;
    if (callback) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, ui);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xE9ECEF), LV_STATE_PRESSED);
        screen_swipe_back_ignore(card);
    } else {
        screen_make_input_passive(card);
    }
    return card;
}

void QuickAction(lv_obj_t* screen, int x, const char* icon, const char* title,
                 lv_event_cb_t callback) {
    auto* button = Button(screen, "", 212, callback, nullptr);
    lv_obj_set_pos(button, x, 560);
    lv_obj_set_height(button, 88);
    auto* glyph = Icon(button, icon, callback ? kText : kMuted);
    lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, 12);
    auto* label = Label(button, title, 200, callback ? kText : kMuted);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, 6, callback ? 48 : 36);
    if (!callback) {
        auto* hint = Label(button, I18n::T("未接入"), 200, kMuted);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 6, 61);
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(button, lv_color_hex(kBg), LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DISABLED);
    }
}
}  // namespace

lv_obj_t* ControlCenterScreen::Create() {
    auto* ui = new Ui{};
    auto* screen = lv_obj_create(nullptr);
    ui->screen = screen;
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, 720, 720);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    auto* back = IconButton(screen, FONT_AWESOME_ANGLE_LEFT, [](lv_event_t*) { Back(); }, nullptr);
    lv_obj_set_pos(back, 28, 28);
    auto* title = Label(screen, "Metalio Control Center", 540);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, 0);
    lv_obj_set_pos(title, 92, 24);
    auto* subtitle = Label(screen, I18n::T("本地设备控制中心"), 530, kMuted);
    lv_obj_set_pos(subtitle, 92, 68);
    auto* settings = IconButton(screen, FONT_AWESOME_GEAR, Configure, ui);
    lv_obj_set_pos(settings, 644, 28);
    auto* overview = Label(screen, I18n::T("设备总览"), 400);
    lv_obj_set_pos(overview, 28, 122);
    ui->device_count = Label(screen, "", 228, kMuted);
    lv_obj_set_style_text_align(ui->device_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(ui->device_count, 464, 122);
    DeviceCard(ui, 28, 160, FONT_AWESOME_MICROCHIP_AI, "Muse Gadget", I18n::T("对话设备"), "",
               Devices, &ui->device_summary, &ui->device_dot, &ui->device_name, &ui->device_role);
    DeviceCard(ui, 366, 160, FONT_AWESOME_USER_ROBOT, "Microduck", "Robot", I18n::T("暂未接入"), nullptr);
    DeviceCard(ui, 28, 336, FONT_AWESOME_COMMENT, "nanoMuse", "Agent", "", Chat,
               &ui->nanomuse_status, &ui->nanomuse_dot);
    lv_obj_t* local_dot = nullptr;
    DeviceCard(ui, 366, 336, FONT_AWESOME_GEAR, I18n::T("本机工具"), "Metalio", "",
               LocalTools, &ui->battery, &local_dot);
    lv_obj_set_style_bg_color(local_dot, lv_color_hex(kOnline), 0);
    auto* actions = Label(screen, I18n::T("快捷操作"), 500);
    lv_obj_set_pos(actions, 28, 522);
    QuickAction(screen, 28, FONT_AWESOME_CAMERA, I18n::T("相机"), OpenCamera);
    QuickAction(screen, 254, FONT_AWESOME_GAMEPAD, I18n::T("机器人"), nullptr);
    QuickAction(screen, 480, FONT_AWESOME_ARROWS_REPEAT, I18n::T("自动化"), nullptr);
    ui->hub_status = Label(screen, "", 400, kMuted);
    lv_label_set_long_mode(ui->hub_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(ui->hub_status, 28, 678);
    auto* configure = Button(screen, I18n::T("连接设置"), 188, Configure, ui);
    lv_obj_set_pos(configure, 440, 664);
    auto* refresh = IconButton(screen, FONT_AWESOME_ARROWS_ROTATE, [](lv_event_t*) {
        DeviceHubClient::GetInstance().Refresh();
    }, nullptr);
    lv_obj_set_pos(refresh, 644, 664);
    Refresh(ui);
    ui->timer = lv_timer_create([](lv_timer_t* timer) {
        Refresh(static_cast<Ui*>(lv_timer_get_user_data(timer)));
    }, 1000, ui);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* state = static_cast<Ui*>(lv_event_get_user_data(e));
        if (state->timer) {
            lv_timer_delete(state->timer);
            state->timer = nullptr;
        }
    }, LV_EVENT_SCREEN_UNLOADED, ui);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* state = static_cast<Ui*>(lv_event_get_user_data(e));
        if (state->timer) lv_timer_delete(state->timer);
        CloseModal(state);
        delete state;
    }, LV_EVENT_DELETE, ui);
    screen_attach_swipe_back(screen, Back);
    return screen;
}

void ControlCenterScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("control_center", event);
    auto& hub = DeviceHubClient::GetInstance();
    if (event == SCREEN_LIFECYCLE_LOAD) hub.Start();
    else {
        hub.Pause();
        NanoMuseChatClient::GetInstance().Pause();
    }
}
