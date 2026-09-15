#include "home_screen.h"
#include "i18n.h"

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <font_awesome.h>

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "IOExpander.hpp"
#include "bq27220_gauge.h"
#include "settings.h"
#include "settings_screen/settings_screen.h"
#include "calculator_screen/calculator_screen.h"
#include "calendar_screen/calendar_screen.h"
#include "call_screen/call_screen.h"
#include "camera_screen/camera_screen.h"
#include "chat_screen/chat_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "gps_screen/gps_screen.h"
#include "level_screen/level_screen.h"
#include "magnet_screen/magnet_screen.h"
#include "music_screen/music_screen.h"
#include "radio_screen/radio_screen.h"
#include "recording_screen/recording_screen.h"
#include "openclaw_screen/openclaw_screen.h"
#include "ai_image_gen_screen/ai_image_gen_screen.h"
#include "translate_screen/translate_screen.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "idle_power_policy.h"
#include "vibrate_screen/vibrate_screen.h"
#include "weather_screen/weather_screen.h"
#include "network_screen/network_screen.h"
#include "pin_test_screen/pin_test_screen.h"
#include "test_screen/test_screen.h"
#include "sd_card_screen/sd_card_screen.h"
#include "theme_screen/theme_screen.h"
#include "info_screen/info_screen.h"
#include "theme_manager.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

// 电池显示模式：1 = 图标（Font Awesome 字形，放最右边），0 = 文字（"电量 NN% 充电中 4.02V"）
#define HOME_STATUS_SHOW_BATTERY_ICON 1

namespace {

constexpr const char* TAG_HOME = "HomeScreen";

// ---------------------------------------------------------------------------
// Per-app lifecycle callbacks
//
// Each launcher hands its callback to screen_attach_lifecycle() so we get a
// LOAD notification right after the new screen becomes active, and an
// UNLOAD notification when LVGL switches away from it.  For now we only log
// the transitions -- but this is the right place to hang start / stop
// behaviour that should track a specific app's lifetime (e.g. pausing the
// audio player when the player screen is dismissed).
//
// All callbacks share the same shape so they can all sit in the AppEntry
// table below.  A nullptr callback simply skips logging for that app.
// ---------------------------------------------------------------------------

void game_2048_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("game_2048", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: game_2048");
    } else {
        ESP_LOGI(TAG_HOME, "unload: game_2048");
    }
}

void calculator_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("calculator", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calculator");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calculator");
    }
}

void call_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("call", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: call_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: call_screen");
    }
    // 转发给 CallScreen 自己处理 PA_SWITCH 切换以及通话兜底挂断。
    CallScreen::LifecycleCallback(event);
}

void calendar_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("calendar", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calendar_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calendar_screen");
    }
}

// 音乐界面的生命周期回调：把 BT 切到模式3、注册 / 摘 UART 回调全部
// 由 MusicScreen::LifecycleCallback 内部统一处理。
void music_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("music", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: music_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: music_screen");
    }
    MusicScreen::LifecycleCallback(event);
}

void radio_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("radio", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: radio_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: radio_screen");
    }
    RadioScreen::LifecycleCallback(event);
}

void recording_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("recording", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: recording_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: recording_screen");
    }
    RecordingScreen::LifecycleCallback(event);
}

void weather_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("weather", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: weather_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: weather_screen");
    }
}

// GPS 屏幕的 GPS_POWER 开关已经搬到 GpsScreen::LifecycleCallback；这里
// 只保留日志 + 转发，与 camera / vibrate / bluetooth 等屏幕的写法对齐。
void gps_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("gps", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: gps_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: gps_screen");
    }
    GpsScreen::LifecycleCallback(event);
}

// 相机生命周期：直接转发到 CameraScreen::LifecycleCallback。
// 那里实现了 CAM_PWDN（TCA9555 IO2，低电平通电）的拉低 / 拉高，以及
// esp_video / V4L2 流水线的启停。这样摄像头只在该 App 处于前台时通电。
void camera_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("camera", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: camera_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: camera_screen");
    }
    CameraScreen::LifecycleCallback(event);
}

// 震动生命周期：转发给 VibrateScreen::LifecycleCallback。
// LOAD 时确保 LEDC 初始化、duty=0；UNLOAD 时停掉 pattern timer 并把 duty=0，
// 保证用户离开屏幕马达不会还在响。
void vibrate_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("vibrate", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: vibrate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: vibrate_screen");
    }
    VibrateScreen::LifecycleCallback(event);
}

// 网络配置：进入页面停掉 WifiStation 并自己接管 STA 栈用于扫描 / 连接，
// 离开时还原。详细逻辑在 NetworkScreen::LifecycleCallback 内。
void wifi_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("network", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: network_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: network_screen");
    }
    NetworkScreen::LifecycleCallback(event);
}

void chat_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("chat", event);
    ChatScreen::LifecycleCallback(event);
}

void digital_people_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("digital_people", event);
    DigitalPeopleScreen::LifecycleCallback(event);
}

// SD 卡生命周期：转发给 SdCardScreen::LifecycleCallback。
// LOAD 时挂载 SD 卡并刷新文件列表，UNLOAD 时安全卸载 SD 卡、
// 断电省电。
void sd_card_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("sd_card", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: sd_card_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: sd_card_screen");
    }
    SdCardScreen::LifecycleCallback(event);
}

// 引脚测试生命周期：屏幕自身会在 UNLOAD 时清掉输入轮询 / 周期方波 timer，
// 这里只多兜底一次以及打 log。
void pin_test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("pin_test", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: pin_test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: pin_test_screen");
    }
    PinTestScreen::LifecycleCallback(event);
}

void test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("test", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: test_screen");
    }
    TestScreen::LifecycleCallback(event);
}

// 水平仪生命周期：转发给 LevelScreen::LifecycleCallback。LOAD 时确保
// SC7A20H 已经 probe + configure 过，UNLOAD 时让 LevelScreen 自己关掉
// sample timer（OnScreenUnloaded 里实现）。
void level_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("level", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: level_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: level_screen");
    }
    LevelScreen::LifecycleCallback(event);
}

// 磁场生命周期：转发给 MagnetScreen::LifecycleCallback。LOAD 时把 QMC6309
// 磁力计 probe + configure 一遍；UNLOAD 由 MagnetScreen 自身在
// OnScreenUnloaded 里关掉采样 timer。
void magnet_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("magnet", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: magnet_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: magnet_screen");
    }
    MagnetScreen::LifecycleCallback(event);
}

// OpenClaw 生命周期：转发给 OpenClawScreen::LifecycleCallback，让屏幕
// 自己负责录音 / 上传任务的兜底关闭与 wake word 恢复。
void openclaw_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("openclaw", event);
    OpenClawScreen::LifecycleCallback(event);
}

void ai_image_gen_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("ai_image_gen", event);
    AiImageGenScreen::LifecycleCallback(event);
}

void translate_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("translate", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: translate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: translate_screen");
    }
    TranslateScreen::LifecycleCallback(event);
}

constexpr int kPanelSize = 720;
constexpr int kStatusBarHeight = 48;      
constexpr int kIndicatorAreaHeight = 40;  
constexpr int kPagerHeight =
    kPanelSize - kStatusBarHeight - kIndicatorAreaHeight;  // 632
constexpr int kAppsPerPage = 9;           // 3x3
constexpr int kPageCols = 3;
constexpr int kPageRows = 3;
constexpr int kIconSize = 128;            // 图标边长；下方再留名称
constexpr int kCellWidth = 160;
constexpr int kNameGap = 6;
constexpr int kNameAreaH = 24;            // font_puhui_20_4 一行高度
constexpr int kCellHeight = kIconSize + kNameGap + kNameAreaH;  // 158
constexpr int kGridColGap = 60;
constexpr int kGridRowGap = 36;
constexpr int kPagePadHor =
    (kPanelSize - kPageCols * kCellWidth - (kPageCols - 1) * kGridColGap) / 2;
constexpr int kPagePadVer =
    (kPagerHeight - kPageRows * kCellHeight - (kPageRows - 1) * kGridRowGap) / 2;

constexpr uint32_t kStatusBarBg = 0x000000;
constexpr int kMaxPages = 6;  // hard cap; bump if app list grows

// Grid descriptors -- static so the array pointers passed to LVGL outlive
// the call.  Initialized at namespace scope; LVGL reads them lazily during
// each page's relayout, so we never have to refresh them.
int32_t s_col_dsc[kPageCols + 1] = {
    kCellWidth,
    kCellWidth,
    kCellWidth,
    LV_GRID_TEMPLATE_LAST,
};
int32_t s_row_dsc[kPageRows + 1] = {
    kCellHeight,
    kCellHeight,
    kCellHeight,
    LV_GRID_TEMPLATE_LAST,
};

// Indicator dot geometry
constexpr int kDotSize = 8;
constexpr int kDotGap = 12;
constexpr int kIndicatorPadHor = 14;
constexpr int kIndicatorPadVer = 8;
constexpr int kIndicatorYOffset = 12;  // distance from panel bottom
constexpr uint32_t kIndicatorBg = 0x000000;
constexpr uint32_t kDotColor = 0xFFFFFF;

// Launchers take the lifecycle callback as an argument so we can attach it
// to the screen object before lv_screen_load() fires LV_EVENT_SCREEN_LOADED
// -- otherwise the first LOAD event would be missed.  Passing nullptr is
// supported (screen_attach_lifecycle no-ops in that case).
typedef void (*LaunchFn)(screen_lifecycle_cb_t lifecycle_cb);

struct AppEntry {
    // 图标资源后缀，例如 "chat"、"2048"。实际 LVGL 路径由 ThemeManager
    // 的当前主题 id 拼出：A:ic_app_home_theme{N}_{suffix}.spng。
    // 这样切换主题只需要重启后重新拼一次路径，不用改 AppEntry。
    const char* icon_suffix;
    const char* name;                    // display name shown under the icon
    LaunchFn launch;                     // tapped -> launch this app (nullptr = no action)
    screen_lifecycle_cb_t lifecycle_cb;  // load / unload observer
};

void LaunchGame2048(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* game = Game2048::Create();
    screen_attach_lifecycle(game, lifecycle_cb);
    lv_screen_load(game);
    if (old_scr != nullptr && old_scr != game) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCalculator(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = Calculator::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCall(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CallScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCalendar(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CalendarScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchMusic(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = MusicScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchRadio(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = RadioScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchRecording(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = RecordingScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchWeather(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = WeatherScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchGps(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = GpsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCamera(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = CameraScreen::Create();
    if (app == nullptr) {
        ESP_LOGE(TAG_HOME, "CameraScreen::Create() failed");
        return;
    }
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchVibrate(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = VibrateScreen::Create();
    if (app == nullptr) {
        ESP_LOGE(TAG_HOME, "VibrateScreen::Create() failed");
        return;
    }
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchWifi(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = NetworkScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchChat(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ChatScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchDigitalPeople(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = DigitalPeopleScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchLevel(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = LevelScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchMagnet(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = MagnetScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchSdCard(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = SdCardScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchPinTest(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = PinTestScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTest(screen_lifecycle_cb_t lifecycle_cb) {
    TestScreen::LaunchFromHome(lifecycle_cb);
}

void LaunchOpenClaw(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    OpenClawScreen::SetLifecycleCallback(lifecycle_cb);
    lv_obj_t* app = OpenClawScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchAiImageGen(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = AiImageGenScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTranslate(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = TranslateScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

// ESPClaw：弹提示 → 将启动分区切到 ota_1 → 重启进入 edge_agent。
bool s_espclaw_switching = false;
lv_obj_t* s_espclaw_overlay = nullptr;
lv_obj_t* s_espclaw_msg_lbl = nullptr;
lv_timer_t* s_espclaw_fail_timer = nullptr;

void StopHomeIdleTimer();  // defined later in this TU

void CloseEspClawPopup() {
    if (s_espclaw_fail_timer != nullptr) {
        lv_timer_delete(s_espclaw_fail_timer);
        s_espclaw_fail_timer = nullptr;
    }
    if (s_espclaw_overlay != nullptr) {
        lv_obj_delete(s_espclaw_overlay);
        s_espclaw_overlay = nullptr;
    }
    s_espclaw_msg_lbl = nullptr;
    s_espclaw_switching = false;
}

void EspClawFailCloseTimer(lv_timer_t* /*timer*/) {
    s_espclaw_fail_timer = nullptr;
    CloseEspClawPopup();
}

void EspClawSwitchFailAsync(void* user_data) {
    const char* msg = static_cast<const char*>(user_data);
    if (s_espclaw_msg_lbl != nullptr && msg != nullptr) {
        lv_label_set_text(s_espclaw_msg_lbl, msg);
    }
    s_espclaw_switching = false;
    if (s_espclaw_fail_timer != nullptr) {
        lv_timer_delete(s_espclaw_fail_timer);
    }
    // 展示错误约 2.5s 后自动收起，便于重试
    s_espclaw_fail_timer =
        lv_timer_create(EspClawFailCloseTimer, 2500, nullptr);
    lv_timer_set_repeat_count(s_espclaw_fail_timer, 1);
}

void EspClawSwitchTask(void* /*arg*/) {
    // 给弹窗一点渲染时间
    vTaskDelay(pdMS_TO_TICKS(1500));

    const esp_partition_t* ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (ota1 == nullptr) {
        ESP_LOGE(TAG_HOME, "ESPClaw: ota_1 partition not found");
        lv_async_call(EspClawSwitchFailAsync,
                      const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(ota1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_HOME, "ESPClaw: set boot to %s failed: %s", ota1->label,
                 esp_err_to_name(err));
        lv_async_call(EspClawSwitchFailAsync,
                      const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGW(TAG_HOME, "ESPClaw: boot -> %s, rebooting", ota1->label);
    Application::GetInstance().Reboot();
    vTaskDelete(nullptr);
}

void ShowEspClawSwitchPopup() {
    lv_obj_t* scr = lv_screen_active();
    if (scr == nullptr) {
        return;
    }
    CloseEspClawPopup();
    s_espclaw_switching = true;

    constexpr int kCardW = 520;
    constexpr int kCardH = 320;

    lv_obj_t* mask = lv_obj_create(scr);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_espclaw_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* head = lv_label_create(card);
    lv_label_set_text(head, "ESPClaw");
    lv_obj_set_style_text_color(head, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* body = lv_label_create(card);
    s_espclaw_msg_lbl = body;
    lv_label_set_text(body, I18n::T("即将进入 ESPClaw..."));
    lv_obj_set_width(body, kCardW - 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
}

void LaunchEspClaw(screen_lifecycle_cb_t /*lifecycle_cb*/) {
    if (s_espclaw_switching) {
        return;
    }
    StopHomeIdleTimer();
    ShowEspClawSwitchPopup();
    if (xTaskCreate(EspClawSwitchTask, "espclaw_sw", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG_HOME, "ESPClaw: failed to create switch task");
        EspClawSwitchFailAsync(
            const_cast<char*>(I18n::T("未找到 ESPClaw\n请确认是否已安装到分区")));
    }
}

void LaunchTheme(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ThemeScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchSettings(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = SettingsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchInfo(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = InfoScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void theme_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("theme", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: theme_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: theme_screen");
    }
    ThemeScreen::LifecycleCallback(event);
}

void settings_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("settings", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: settings_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: settings_screen");
    }
    SettingsScreen::LifecycleCallback(event);
}

void info_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("info", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: info_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: info_screen");
    }
    InfoScreen::LifecycleCallback(event);
}

// 主屏 app 表。icon_suffix 对应 ic_app_home_theme{N}_{suffix}.spng 的
// 中间 suffix 部分；name 显示在图标正下方（传统手机桌面样式）。
// 注意 "定位" 的 suffix 是 "gps"（资源命名），不是 "map"；"磁场" 暂无
// 主题图标，资源 ic_app_home_themeN_magnet.spng 缺失时该格会显示为空。
// name 存 zh-CN msgid（源文案）；显示时用 I18n::T(entry.name)。
constexpr AppEntry kApps[] = {
    {"chat",           "聊天",     LaunchChat,          chat_lifecycle_cb},
    {"wifi",           "网络配置", LaunchWifi,          wifi_lifecycle_cb},
    {"digital_people", "数字人",   LaunchDigitalPeople, digital_people_lifecycle_cb},
    {"call",           "电话",     LaunchCall,          call_lifecycle_cb},
    {"music",          "音乐",     LaunchMusic,         music_lifecycle_cb},
    {"calendar",       "日历",     LaunchCalendar,      calendar_lifecycle_cb},
    {"openclaw",       "OpenClaw", LaunchOpenClaw,      openclaw_lifecycle_cb},
    {"espclaw",       "ESPClaw",     LaunchEspClaw,     nullptr},
    {"camera",         "相机",     LaunchCamera,        camera_lifecycle_cb},
    {"gps",            "定位",     LaunchGps,           gps_lifecycle_cb},
    {"spirit_level",   "水平仪",   LaunchLevel,         level_lifecycle_cb},
    {"magnet",         "磁场",     LaunchMagnet,        magnet_lifecycle_cb},
    {"vibrate",        "震动",     LaunchVibrate,       vibrate_lifecycle_cb},
    {"calculator",     "计算器",   LaunchCalculator,    calculator_lifecycle_cb},
    {"weather",        "天气",     LaunchWeather,       weather_lifecycle_cb},
    {"sd",             "SD卡",     LaunchSdCard,        sd_card_lifecycle_cb},
    {"pin",            "引脚测试", LaunchPinTest,       pin_test_lifecycle_cb},
    {"2048",           "2048",     LaunchGame2048,      game_2048_lifecycle_cb},
    {"info",           "系统信息", LaunchInfo,          info_lifecycle_cb},
    {"theme",          "主题",     LaunchTheme,         theme_lifecycle_cb},
    {"test",           "测试",     LaunchTest,          test_lifecycle_cb},
    {"settings",       "设置",     LaunchSettings,      settings_lifecycle_cb},
    {"radio",       "电台",     LaunchRadio,      radio_lifecycle_cb},
    {"recording",       "录音",     LaunchRecording,      recording_lifecycle_cb},
    {"ai_image_gen",       "AI生图",     LaunchAiImageGen,      ai_image_gen_lifecycle_cb},
    {"translate",       "翻译",     LaunchTranslate,      translate_lifecycle_cb},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

// 当前主题下，每个 app 解析出来的完整 LVGL 路径。在 HomeScreen::Create()
// 入口处由 EnsureIconPathsBuilt() 填写；主题 id 变化时会自动重新拼接。
//
// 路径字符串必须在 lv_image 引用期间一直有效（lv_image_set_src 不复制内
// 存），所以放在 namespace 静态而不是栈或函数局部。
constexpr int kIconPathBufSize = 56;
char s_icon_paths[kTotalApps][kIconPathBufSize];

void EnsureIconPathsBuilt() {
    static bool built = false;
    static int s_built_theme_id = 0;
    const int tid = ThemeManager::GetCurrentThemeId();
    // 第一次构建 / 主题 id 变更（理论上只在调试热改 NVS 时出现），都重新拼。
    if (built && s_built_theme_id == tid) {
        return;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      tid, kApps[i].icon_suffix);
    }
    s_built_theme_id = tid;
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", tid);
}

// ---------------------------------------------------------------------------
// 主屏触摸：一次「无→有→无」为一段会话，screen 级统一分类与分发。
// 不再使用 cell 的 LV_EVENT_CLICKED。
// ---------------------------------------------------------------------------

constexpr int kHomeMoveThreshold = 5;       // 滑动激活 / 点击位移上限（|dx|、|dy| 均 < 此值才算 tap）
constexpr int kHomeAxisLockThreshold = 12;  // 消抖：主方向位移超过此值才锁定手势
constexpr int kPageSnapThreshold = kPanelSize / 5;  // 跟手拖动超过 20% 页宽即切页（720→144px）
constexpr int kHomeFlickThreshold = 24;       // 松手快速 fling 位移阈值
constexpr uint32_t kHomeLongPressMs = 750;  // 按下→松开 ≥ 此值且位移够小 → 长按
constexpr uint32_t kPageSlideAnimMs = 300;  // 左右翻页吸附动画时长

constexpr lv_obj_flag_t kAppCellFlag = LV_OBJ_FLAG_USER_2;

enum class HomeTouchKind {
    None,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Click,
    LongPress,
};

enum class HomeGestureAxis {
    None,
    Horizontal,
    Vertical,
};

struct HomeTouchSession {
    bool active = false;
    bool consumed = false;  // 已由滑动手势消费
    bool paging = false;      // 水平跟手拖动翻页中
    HomeGestureAxis axis = HomeGestureAxis::None;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;       // 跟手拖动时的上一采样点
    uint32_t press_tick = 0;
    lv_obj_t* press_cell = nullptr;
    const AppEntry* app = nullptr;
};
HomeTouchSession s_home_touch;

// ---------------------------------------------------------------------------
// 主屏无操作计时：由 idle_power_policy 统一管理（仅进入待机，不自动关机）。
// ---------------------------------------------------------------------------
void BeginSystemShutdown(const char* reason);

void ResetHomeIdleTimer() { IdlePower_NotifyActivity(); }

void StopHomeIdleTimer() { IdlePower_Detach(IdlePowerSession::Home); }

void StartHomeIdleTimer() {
    IdlePower_Attach(IdlePowerSession::Home, /*reset_activity=*/true);
}

// App 卡片按压过渡：确认点击/长按后触发缩放。
constexpr uint32_t kCellPressScaleMs = 200;  // 与 GetPressTransition 时长一致

const lv_style_prop_t kPressTransProps[] = {
    LV_STYLE_TRANSFORM_SCALE_X,
    LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_PROP_INV,
};

lv_style_transition_dsc_t& GetPressTransition() {
    static lv_style_transition_dsc_t dsc;
    static bool inited = false;
    if (!inited) {
        lv_style_transition_dsc_init(&dsc, kPressTransProps, lv_anim_path_ease_out,
                                     kCellPressScaleMs, 0, nullptr);
        inited = true;
    }
    return dsc;
}

void LaunchHomeApp(const AppEntry* app) {
    if (app != nullptr && app->launch != nullptr) {
        app->launch(app->lifecycle_cb);
    }
}

struct CellScaleCtx {
    lv_obj_t* cell = nullptr;
    const AppEntry* launch_app = nullptr;  // 非空：缩放结束后进入 app
};

lv_timer_t* s_cell_scale_timer = nullptr;
CellScaleCtx s_cell_scale_ctx;

void CancelCellScaleTimer() {
    if (s_cell_scale_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_cell_scale_timer);
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
}

void OnCellScaleTimer(lv_timer_t* timer) {
    const CellScaleCtx ctx = s_cell_scale_ctx;
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
    lv_timer_delete(timer);

    if (ctx.launch_app != nullptr) {
        LaunchHomeApp(ctx.launch_app);
        return;
    }
    if (ctx.cell != nullptr) {
        lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
    }
}

// launch_app：单击传入 app，缩放 kCellPressScaleMs 后再 launch；长按传 nullptr，仅缩放回弹。
void PlayAppCellPressScale(lv_obj_t* cell, const AppEntry* launch_app) {
    if (cell == nullptr) {
        return;
    }
    CancelCellScaleTimer();
    lv_obj_add_state(cell, LV_STATE_PRESSED);
    s_cell_scale_ctx.cell = cell;
    s_cell_scale_ctx.launch_app = launch_app;
    s_cell_scale_timer =
        lv_timer_create(OnCellScaleTimer, kCellPressScaleMs, nullptr);
    lv_timer_set_repeat_count(s_cell_scale_timer, 1);
}

lv_obj_t* FindAppCellFromTarget(lv_obj_t* target, lv_obj_t* screen) {
    for (lv_obj_t* obj = target; obj != nullptr && obj != screen;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kAppCellFlag)) {
            return obj;
        }
    }
    return nullptr;
}

lv_obj_t* CreateAppCellSkeleton(lv_obj_t* cell) {
    // 翻页骨架：不透明直角。PPA 开启时半透明/圆角会先 msync 再软件 fallback，
    // 易刷 invalid addr；静止态图标仍保留圆角。尺寸与图标一致；骨架态隐藏名称。
    constexpr uint32_t kSkeletonBg = 0x2A2F3A;

    lv_obj_t* skeleton = lv_obj_create(cell);
    lv_obj_remove_style_all(skeleton);
    lv_obj_set_size(skeleton, kIconSize, kIconSize);
    lv_obj_set_style_bg_color(skeleton, lv_color_hex(kSkeletonBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(skeleton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(skeleton, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(skeleton, 0, LV_PART_MAIN);
    lv_obj_align(skeleton, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
    return skeleton;
}

lv_obj_t* CreateAppCell(lv_obj_t* parent, const AppEntry& entry, int idx) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, kCellWidth, kCellHeight);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);

    // 单元格：上图标、下名称；不对整格 clip，避免裁切底部文字。
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cell, false, LV_PART_MAIN);
    lv_obj_set_style_transition(cell, &GetPressTransition(), LV_PART_MAIN);

    // 缩放绕图标中心；仅在 screen 确认 Click / LongPress 后 add_state(PRESSED)。
    lv_obj_set_style_transform_pivot_x(cell, kCellWidth / 2,
                                       LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_y(cell, kIconSize / 2,
                                       LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(cell, 262, LV_PART_MAIN | LV_STATE_PRESSED);

    // 路径取自 s_icon_paths[idx]：已按当前主题前缀解析好（ic_app_home_themeN_xxx）。
    lv_obj_t* icon = lv_image_create(cell);
    lv_image_set_src(icon, s_icon_paths[idx]);
    lv_obj_set_size(icon, kIconSize, kIconSize);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    // child[1]：翻页骨架（与 icon 同位同尺寸），见 SetPagerSkeletonMode。
    CreateAppCellSkeleton(cell);

    lv_obj_t* name = lv_label_create(cell);
    lv_label_set_text(name, entry.name != nullptr ? I18n::T(entry.name) : "");
    lv_obj_set_width(name, kCellWidth);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, kIconSize + kNameGap);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    if (entry.launch != nullptr) {
        // 可命中以便 PRESSED 时锁定 app；不用 LV_EVENT_CLICKED，由 screen 分发。
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cell, kAppCellFlag);
        lv_obj_set_user_data(cell, const_cast<AppEntry*>(&entry));
    }
    return cell;
}

// ---------------------------------------------------------------------------
// Pager + page-indicator
//
// Layout
//   pager (720 x kPagerHeight) -- horizontal scroll, snap to page center.
//     each child is a Page object (also 720 x kPagerHeight) which holds a
//     3x3 grid of cells.  We mark every page LV_OBJ_FLAG_SNAPPABLE so a
//     swipe ends with one page perfectly centered.
//
// State
//   PagerState lives on the heap and is owned by the screen via
//   LV_EVENT_DELETE.  The scroll callback uses it to map scroll position
//   -> current page and re-tint the dots.  No globals; if HomeScreen is
//   torn down and rebuilt, the new instance allocates a fresh state.
// ---------------------------------------------------------------------------

struct PagerState {
    lv_obj_t* pager;
    lv_obj_t* indicator = nullptr;  // 底部页码胶囊；翻页中会临时隐藏
    lv_obj_t* dots[kMaxPages];
    int page_count;
    int current_page;
    bool skeleton_active = false;
};

// 记住用户上次停留在主屏的哪一页，便于从子页面返回时回到同一页。
// 子页面调用 HomeScreen::Create() 重建时，会读取这个值滚动到对应页。
// 用户每次翻页（GoToPage）会写入；HighlightDot 不写入，否则
// CreateIndicator 初始化时调用 HighlightDot(state, 0) 会把它清零。
int s_last_home_page = 0;

struct HomeStatusState {
    lv_obj_t* bar = nullptr;
    lv_obj_t* network_icon_lbl = nullptr;
    lv_obj_t* network_type_lbl = nullptr;
    lv_obj_t* sim_slot_lbl = nullptr;       // 仅 4G 模式下显示：外置卡 / 内置卡
    lv_obj_t* battery_icon_lbl = nullptr;   // 电池图标（Font Awesome 字形）
    lv_obj_t* battery_pct_lbl  = nullptr;   // 电池电量文字，例如 I18n::T("电量 85%")
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* activation_code_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;
    const char* last_icon = nullptr;
    const char* last_battery_icon = nullptr; // 缓存上次图标，避免重复 set_text
    int  last_battery_pct = -1;
    bool last_battery_low = false;  // 缓存上一次的低电量染色状态
    int  last_net_type = -1;        // 缓存上次显示的网络类型，避免每秒重绘 SIM 标签
    int  last_sim_slot = -1;        // 缓存上次显示的 SIM 槽位
    std::string last_activation_text;  // 缓存验证码文案，避免每秒 invalidate
    bool last_activation_visible = false;
};

HomeStatusState* s_home_status = nullptr;

void SetPagerSkeletonMode(PagerState* state, bool active) {
    if (state == nullptr || state->pager == nullptr ||
        state->skeleton_active == active) {
        return;
    }
    state->skeleton_active = active;

    // PPA 对半透明 fill 会先 msync 再软件绘制；翻页重绘风暴里极易踩到
    // 非 cacheable 中间层。翻页期间把半透明装饰关掉/改不透明。
    if (state->indicator != nullptr) {
        if (active) {
            lv_obj_add_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_home_status != nullptr && s_home_status->bar != nullptr) {
        lv_obj_set_style_bg_opa(s_home_status->bar,
                                active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }

    const uint32_t page_child_count = lv_obj_get_child_count(state->pager);
    for (uint32_t p = 0; p < page_child_count; ++p) {
        lv_obj_t* page = lv_obj_get_child(state->pager, p);
        if (page == nullptr) {
            continue;
        }
        const uint32_t cell_count = lv_obj_get_child_count(page);
        for (uint32_t c = 0; c < cell_count; ++c) {
            lv_obj_t* cell = lv_obj_get_child(page, c);
            if (cell == nullptr || !lv_obj_has_flag(cell, kAppCellFlag)) {
                continue;
            }
            // child[0]=icon, [1]=skeleton, [2]=name
            lv_obj_t* icon = lv_obj_get_child(cell, 0);
            lv_obj_t* skeleton = lv_obj_get_child(cell, 1);
            lv_obj_t* name = lv_obj_get_child(cell, 2);
            if (icon == nullptr || skeleton == nullptr) {
                continue;
            }
            if (active) {
                lv_obj_remove_state(cell, LV_STATE_PRESSED);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

void UpdateHomeStatusBar(HomeStatusState* st);

int GetSavedNetworkType() {
    // 与 DualNetworkBoard / network_screen 共用 NVS 读取逻辑
    constexpr int kDefaultNetType = 1;  // 默认 4G，与 metalio-claw-4 板级一致
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(kDefaultNetType);
    return type == NetworkType::ML307 ? 1 : 0;
}

// 与 network_screen 共用 "network/sim_slot" 这一 NVS key。
// 返回 0 = 外置卡（默认）/ 1 = 内置卡。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", 0);
    return (v == 1) ? 1 : 0;
}

void SaveSimSlot(int slot) {
    Settings settings("network", true);
    settings.SetInt("sim_slot", slot);
}

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

// 开机向模组查询一次当前 SIM 槽位，写回 NVS 并刷新状态栏。
bool s_boot_sim_slot_query_done = false;

int ParseSimSlotFromEcsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) {
            return -1;
        }
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) {
            ++i;
        }
        if (i >= resp.size() ||
            !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() &&
               std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

struct BootSimSlotQueryMsg {
    int slot = -1;
};

void AsyncBootSimSlotSynced(void* user_data) {
    auto* msg = static_cast<BootSimSlotQueryMsg*>(user_data);
    if (msg->slot >= 0 && GetSavedSimSlot() != msg->slot) {
        SaveSimSlot(msg->slot);
        ESP_LOGI(TAG_HOME, "sim_slot synced from modem at boot: %d", msg->slot);
    }
    if (s_home_status != nullptr) {
        s_home_status->last_sim_slot = -1;
        UpdateHomeStatusBar(s_home_status);
    }
    delete msg;
}

void BootSimSlotQueryTask(void* /*arg*/) {
    auto* msg = new BootSimSlotQueryMsg{};
    Nt26Board* nt26 = GetNt26Board();
    if (nt26 != nullptr) {
        std::string resp;
        esp_err_t err = nt26->SendAtCommand("AT+ECSIMCFG?", resp, 5000,
                                            /*bypass_init_check=*/true);
        ESP_LOGI(TAG_HOME, "boot AT+ECSIMCFG? -> err=%d resp_len=%u",
                 (int)err, (unsigned)resp.size());
        if (err == ESP_OK && resp.find("OK") != std::string::npos) {
            const int slot = ParseSimSlotFromEcsimcfg(resp);
            if (slot == 0 || slot == 1) {
                msg->slot = slot;
            } else {
                ESP_LOGW(TAG_HOME, "boot ECSIMCFG: SimSlot not parsed, resp='%s'",
                         resp.c_str());
            }
        }
    }
    lv_async_call(AsyncBootSimSlotSynced, msg);
    vTaskDelete(nullptr);
}

void ScheduleBootSimSlotQuery() {
    if (s_boot_sim_slot_query_done) {
        return;
    }
    if (GetSavedNetworkType() != 1) {
        return;
    }
    if (GetNt26Board() == nullptr) {
        return;
    }
    s_boot_sim_slot_query_done = true;
    if (xTaskCreate(BootSimSlotQueryTask, "home_sim_q", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        s_boot_sim_slot_query_done = false;
        ESP_LOGE(TAG_HOME, "xTaskCreate(home_sim_q) failed");
    }
}

void UpdateHomeStatusBar(HomeStatusState* st) {
    if (st == nullptr || st->bar == nullptr) {
        return;
    }

    const int net_type = GetSavedNetworkType();
    if (st->network_type_lbl != nullptr) {
        lv_label_set_text(st->network_type_lbl, net_type == 1 ? "4G" : "WiFi");
    }

    // SIM 卡名称：4G 模式下显示「外置卡 / 内置卡」，WiFi 模式下整个标签隐藏。
    // 只在内容真正变化时更新，省一次 invalidate。
    if (st->sim_slot_lbl != nullptr) {
        if (net_type == 1) {
            const int slot = GetSavedSimSlot();
            if (st->last_net_type != net_type || st->last_sim_slot != slot) {
                lv_label_set_text(st->sim_slot_lbl,
                                  slot == 1 ? I18n::T("内置卡") : I18n::T("外置卡"));
                st->last_sim_slot = slot;
            }
            lv_obj_remove_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (st->last_net_type != net_type) {
                lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    st->last_net_type = net_type;

    const char* icon = Board::GetInstance().GetNetworkStateIcon();
    if (icon != nullptr && st->network_icon_lbl != nullptr &&
        icon != st->last_icon) {
        st->last_icon = icon;
        lv_label_set_text(st->network_icon_lbl, icon);
    }

    // ---- 电池 ----
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 图标模式：仅 Font Awesome 电池图标，靠最右边
    if (st->battery_icon_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const char* bat_icon = nullptr;
            if (charging) {
                bat_icon = FONT_AWESOME_BATTERY_BOLT;
            } else if (battery_level >= 80) {
                bat_icon = FONT_AWESOME_BATTERY_FULL;
            } else if (battery_level >= 60) {
                bat_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
            } else if (battery_level >= 40) {
                bat_icon = FONT_AWESOME_BATTERY_HALF;
            } else if (battery_level >= 20) {
                bat_icon = FONT_AWESOME_BATTERY_QUARTER;
            } else {
                bat_icon = FONT_AWESOME_BATTERY_EMPTY;
            }
            if (bat_icon != st->last_battery_icon) {
                st->last_battery_icon = bat_icon;
                lv_label_set_text(st->battery_icon_lbl, bat_icon);
            }

            const bool low = !charging && battery_level < 20;
            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            // 无电池：斜杠图标，恢复白色
            st->last_battery_icon = FONT_AWESOME_BATTERY_SLASH;
            lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_SLASH);
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#else
    // 文字模式：电量百分比 + 充电中 + 电压（原有行为）
    if (st->battery_pct_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        bool has_battery = Board::GetInstance().GetBatteryLevel(
            battery_level, charging, discharging);

        if (has_battery) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const bool low = !charging && battery_level < 20;

            char buf[48];
            uint16_t dbg_mv = 0;
            char volt_str[16];
            if (Bq27220Gauge::GetInstance().GetVoltageMv(dbg_mv)) {
                std::snprintf(volt_str, sizeof(volt_str), "%.2fV",
                              dbg_mv / 1000.0f);
            } else {
                std::snprintf(volt_str, sizeof(volt_str), "--V");
            }
            if (charging) {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% 充电中 %s"),
                              battery_level, volt_str);
            } else {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% %s"),
                              battery_level, volt_str);
            }
            lv_label_set_text(st->battery_pct_lbl, buf);
            st->last_battery_pct = battery_level;

            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            if (st->last_battery_pct != -1) {
                st->last_battery_pct = -1;
                lv_label_set_text(st->battery_pct_lbl, I18n::T("电量 --%"));
            }
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#endif

    if (st->time_lbl != nullptr) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr &&
            tm_info.tm_year >= 2025 - 1900) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
            lv_label_set_text(st->time_lbl, time_str);
        } else {
            lv_label_set_text(st->time_lbl, "--:--");
        }
    }

    if (st->activation_code_lbl != nullptr) {
        auto& app = Application::GetInstance();
        if (app.HasPendingActivation()) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), I18n::T("验证码: %s"),
                          app.GetPendingActivationCode().c_str());
            if (st->last_activation_text != buf) {
                st->last_activation_text = buf;
                lv_label_set_text(st->activation_code_lbl, buf);
            }
            if (!st->last_activation_visible) {
                st->last_activation_visible = true;
                lv_obj_remove_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (st->last_activation_visible) {
            st->last_activation_visible = false;
            st->last_activation_text.clear();
            lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OnHomeStatusTimer(lv_timer_t* timer) {
    UpdateHomeStatusBar(static_cast<HomeStatusState*>(lv_timer_get_user_data(timer)));
}

void OnHomeStatusDeleted(lv_event_t* e) {
    auto* st = static_cast<HomeStatusState*>(lv_event_get_user_data(e));
    if (st == nullptr) {
        return;
    }
    if (s_home_status == st) {
        s_home_status = nullptr;
    }
    if (st->update_timer != nullptr) {
        lv_timer_delete(st->update_timer);
        st->update_timer = nullptr;
    }
    delete st;
}

lv_obj_t* CreateStatusBar(lv_obj_t* screen, HomeStatusState* st) {
    // 左右两块容器宽度直接锁死，避免 LVGL flex 在 SIZE_CONTENT + SPACE_BETWEEN
    // 组合下对内容宽度的二次评估把右边「电量 100% 充电中」末尾几个字裁掉。
    //   - 左侧 300px：足够放下「图标 + 4G + 外置卡」
    //   - 右侧 400px：「电量 100% 充电中 HH:MM」是 ~18 字宽，预留充分
    //   - 中间空隙由父 flex SPACE_BETWEEN 自动撑开
    constexpr int kStatusLeftWidth  = 300;
    constexpr int kStatusRightWidth = 400;

    lv_obj_t* bar = lv_obj_create(screen);
    st->bar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kPanelSize, kStatusBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kStatusBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 8, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kStatusLeftWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, LV_PART_MAIN);

    st->network_icon_lbl = lv_label_create(left);
    lv_label_set_text(st->network_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(st->network_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    st->network_type_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->network_type_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->network_type_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->network_type_lbl, "WiFi");
    lv_obj_set_style_text_font(st->network_type_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_type_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    // SIM 卡标签（外置卡 / 内置卡），用稍浅一些的灰色与「4G」做视觉区分。
    // 默认隐藏，UpdateHomeStatusBar 会根据当前网络类型决定显隐。
    st->sim_slot_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->sim_slot_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->sim_slot_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->sim_slot_lbl, "");
    lv_obj_set_style_text_font(st->sim_slot_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sim_slot_lbl, lv_color_hex(0xC9D1D9),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);

    // ---- 状态栏右侧：电池电量文字（时间单独居中浮在状态栏正中） ----
    // 故意不再放 font_awesome 电池图标，避免字符渲染不到 / 占位问题。
    // 右容器宽度固定 400px，靠右排电量文字，这样无论文本变长
    // （"电量 100% 充电中"）还是变短（"电量 --%"）都不会被父级 flex
    // 重排时挤压裁切。
    lv_obj_t* right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, kStatusRightWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 图标模式：只放一个 Font Awesome 电池图标，靠最右边
    st->battery_icon_lbl = lv_label_create(right);
    lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#else
    // 文字模式：电量百分比 + 充电中 + 电压（原有行为）
    lv_obj_set_style_pad_column(right, 14, LV_PART_MAIN);

    st->battery_pct_lbl = lv_label_create(right);
    lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->battery_pct_lbl, 380);
    lv_label_set_text(st->battery_pct_lbl, I18n::T("电量 --%"));
    lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#endif

    // 时间 + 激活码居中：FLOATING 容器脱离 bar 的 flex 布局，整体锚在
    // 状态栏正中央；未激活时激活码显示在时间右侧，激活成功后隐藏。
    lv_obj_t* center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center, 8, LV_PART_MAIN);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    st->time_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->time_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->time_lbl, 80);
    lv_label_set_text(st->time_lbl, "--:--");
    lv_obj_set_style_text_align(st->time_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->time_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->activation_code_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->activation_code_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->activation_code_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->activation_code_lbl, "");
    lv_obj_set_style_text_font(st->activation_code_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->activation_code_lbl, lv_color_hex(0xFBBF24),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);

    UpdateHomeStatusBar(st);
    st->update_timer = lv_timer_create(OnHomeStatusTimer, 1000, st);
    s_home_status = st;
    ScheduleBootSimSlotQuery();

    lv_obj_add_event_cb(screen, OnHomeStatusDeleted, LV_EVENT_DELETE, st);
    return bar;
}

void HighlightDot(PagerState* state, int page) {
    if (page < 0 || page >= state->page_count) {
        return;
    }
    state->current_page = page;
    for (int i = 0; i < state->page_count; ++i) {
        // Active dot is fully opaque, idle dots are subtle.  We keep both
        // the color the same so the row reads as a connected element.
        lv_opa_t opa = (i == page) ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_bg_opa(state->dots[i], opa, LV_PART_MAIN);
    }
}

bool PagerLoopEnabled(const PagerState* state) {
    return state != nullptr && state->page_count > 1;
}

// 无限循环翻页：pager 首尾各加一页克隆，布局为
// [末页克隆][真实页0..N-1][首页克隆]，真实页 i 的 scroll_x = (i+1)*kPanelSize。
int32_t PagerScrollXForPage(const PagerState* state, int logical_page) {
    if (state == nullptr) {
        return 0;
    }
    const int physical = PagerLoopEnabled(state) ? logical_page + 1 : logical_page;
    return static_cast<int32_t>(physical) * kPanelSize;
}

void PagerMaybeWrapAfterScroll(PagerState* state) {
    if (!PagerLoopEnabled(state) || state->pager == nullptr) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (scroll_x == 0) {
        const int last = state->page_count - 1;
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, last),
                           LV_ANIM_OFF);
        HighlightDot(state, last);
        s_last_home_page = last;
    } else if (scroll_x == static_cast<int32_t>(state->page_count + 1) * kPanelSize) {
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, 0),
                           LV_ANIM_OFF);
        HighlightDot(state, 0);
        s_last_home_page = 0;
    }
}

void OnPagerScrollBegin(lv_event_t* e) {
    lv_anim_t* a = lv_event_get_scroll_anim(e);
    if (a == nullptr) {
        return;
    }
    lv_anim_set_duration(a, kPageSlideAnimMs);
    lv_anim_set_path_cb(a, lv_anim_path_ease_out);
}

void OnPagerScrollEnd(lv_event_t* e) {
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    // 跟手拖动时 scroll_by(LV_ANIM_OFF) 每帧都会触发 SCROLL_END，
    // 不能在此处关骨架，否则手动拖时永远看不到骨架效果。
    if (s_home_touch.active && s_home_touch.paging) {
        return;
    }
    if (!lv_obj_is_scrolling(state->pager)) {
        PagerMaybeWrapAfterScroll(state);
        SetPagerSkeletonMode(state, false);
    }
}

// 翻页：平滑滚动到目标页；跟手拖动松手后也会调用以吸附到最近页。
void GoToPage(PagerState* state, int target_page) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    if (target_page < 0 || target_page >= state->page_count) {
        if (!PagerLoopEnabled(state)) {
            return;
        }
        target_page =
            (target_page % state->page_count + state->page_count) % state->page_count;
    }

    const int current = state->current_page;
    int32_t target_x = PagerScrollXForPage(state, target_page);
    if (PagerLoopEnabled(state)) {
        if (target_page == 0 && current == state->page_count - 1) {
            target_x = static_cast<int32_t>(state->page_count + 1) * kPanelSize;
        } else if (target_page == state->page_count - 1 && current == 0) {
            target_x = 0;
        }
    }

    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (target_page == current && scroll_x == target_x) {
        SetPagerSkeletonMode(state, false);
        return;
    }
    SetPagerSkeletonMode(state, true);
    lv_obj_scroll_to_x(state->pager, target_x, LV_ANIM_ON);
    HighlightDot(state, target_page);
    s_last_home_page = target_page;
    ResetHomeIdleTimer();
}

void SnapPagerToNearestPage(PagerState* state, int release_dx) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    const int anchor_x = PagerScrollXForPage(state, state->current_page);
    const int delta = static_cast<int>(scroll_x) - anchor_x;

    int target = state->current_page;
    // 相对起始页对称判定：左滑 delta>0 进下一页，右滑 delta<0 回上一页。
    if (delta > kPageSnapThreshold ||
        (release_dx <= -kHomeFlickThreshold && delta > kHomeMoveThreshold)) {
        target = state->current_page + 1;
    } else if (delta < -kPageSnapThreshold ||
               (release_dx >= kHomeFlickThreshold && delta < -kHomeMoveThreshold)) {
        target = state->current_page - 1;
    }

    if (target < 0) {
        if (PagerLoopEnabled(state) && state->current_page == 0) {
            GoToPage(state, state->page_count - 1);
            return;
        }
        target = 0;
    }
    if (target >= state->page_count) {
        if (PagerLoopEnabled(state) &&
            state->current_page == state->page_count - 1) {
            GoToPage(state, 0);
            return;
        }
        target = state->page_count - 1;
    }
    GoToPage(state, target);
}

// ---------------------------------------------------------------------------
// 触摸会话：PRESSED 锁定目标 → PRESSING/RELEASED 判滑动 → RELEASED 判单击/长按
//
//   |dx|、|dy| 均 < kHomeMoveThreshold：
//     按下→松开 < kHomeLongPressMs → Click（缩放 + launch）
//     按下→松开 ≥ kHomeLongPressMs → LongPress（仅缩放，预留）
//   |dx| >= |dy| 且主方向位移 >= kHomeAxisLockThreshold → 左右滑（PRESSING 跟手 / RELEASED 吸附）
//   |dy| > |dx| 且主方向位移 >= kHomeAxisLockThreshold → 上下滑（忽略，不隐藏图标）
//   方向未锁定前（消抖区）→ 不切换骨架、不跟手翻页
// ---------------------------------------------------------------------------

bool HomeTouchIsHorizontalSlide(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return adx >= kHomeAxisLockThreshold && adx * 2 > ady * 3;
}

bool HomeTouchIsVerticalSlide(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return ady >= kHomeAxisLockThreshold && ady * 2 > adx * 3;
}

void HomeTouchUpdateAxisLock(int dx, int dy) {
    if (s_home_touch.axis != HomeGestureAxis::None) {
        return;
    }
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Horizontal;
        return;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Vertical;
        s_home_touch.consumed = true;
    }
}

bool HomeTouchIsTapLike(int dx, int dy) {
    return std::abs(dx) < kHomeMoveThreshold && std::abs(dy) < kHomeMoveThreshold;
}

HomeTouchKind HomeTouchClassifySwipe(int dx, int dy) {
    if (HomeTouchIsTapLike(dx, dy)) {
        return HomeTouchKind::None;
    }
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        return dx < 0 ? HomeTouchKind::SwipeLeft : HomeTouchKind::SwipeRight;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        return dy < 0 ? HomeTouchKind::SwipeUp : HomeTouchKind::SwipeDown;
    }
    return HomeTouchKind::None;
}

void HomeTouchHandleSwipe(PagerState* state, HomeTouchKind kind) {
    if (state == nullptr) {
        return;
    }
    switch (kind) {
        case HomeTouchKind::SwipeLeft:
            GoToPage(state, state->current_page + 1);
            break;
        case HomeTouchKind::SwipeRight:
            GoToPage(state, state->current_page - 1);
            break;
        case HomeTouchKind::SwipeUp:
        case HomeTouchKind::SwipeDown:
            break;
        default:
            break;
    }
}

void HomeTouchDispatchTapLike(HomeTouchKind kind) {
    if (s_home_touch.press_cell == nullptr) {
        return;
    }
    if (kind == HomeTouchKind::Click) {
        if (s_home_touch.app == nullptr) {
            return;
        }
        PlayAppCellPressScale(s_home_touch.press_cell, s_home_touch.app);
        return;
    }
    if (kind == HomeTouchKind::LongPress) {
        PlayAppCellPressScale(s_home_touch.press_cell, nullptr);
    }
}

void HomeTouchTryStartPageDrag(PagerState* state, int dx, int dy, int current_x) {
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
        return;
    }
    if (s_home_touch.consumed && !s_home_touch.paging) {
        return;
    }
    if (!HomeTouchIsHorizontalSlide(dx, dy)) {
        return;
    }
    if (!s_home_touch.paging) {
        s_home_touch.paging = true;
        s_home_touch.consumed = true;
        if (state != nullptr && state->pager != nullptr) {
            lv_obj_stop_scroll_anim(state->pager);
            SetPagerSkeletonMode(state, true);
        }
        s_home_touch.last_x = static_cast<int16_t>(current_x);
    }
}

void OnHomePressed(lv_event_t* e) {
    if (s_home_touch.active) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t* screen = lv_event_get_current_target_obj(e);
    lv_obj_t* cell = FindAppCellFromTarget(lv_event_get_target_obj(e), screen);

    s_home_touch.active = true;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.start_x = static_cast<int16_t>(p.x);
    s_home_touch.start_y = static_cast<int16_t>(p.y);
    s_home_touch.last_x = static_cast<int16_t>(p.x);
    s_home_touch.press_tick = lv_tick_get();
    s_home_touch.press_cell = cell;
    s_home_touch.app =
        cell != nullptr
            ? static_cast<const AppEntry*>(lv_obj_get_user_data(cell))
            : nullptr;
    ResetHomeIdleTimer();
}

void OnHomePressing(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = p.x - s_home_touch.start_x;
    const int dy = p.y - s_home_touch.start_y;
    if (!HomeTouchIsTapLike(dx, dy)) {
        ResetHomeIdleTimer();
    }

    HomeTouchUpdateAxisLock(dx, dy);

    // 上下滑或消抖区：不隐藏图标、不跟手翻页。
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
        return;
    }

    if (state != nullptr) {
        SetPagerSkeletonMode(state, true);
    }

    if (!s_home_touch.consumed || s_home_touch.paging) {
        HomeTouchTryStartPageDrag(state, dx, dy, p.x);
    }

    if (s_home_touch.paging && state != nullptr && state->pager != nullptr) {
        const int delta_x = p.x - s_home_touch.last_x;
        if (delta_x != 0) {
            lv_obj_scroll_by(state->pager, delta_x, 0, LV_ANIM_OFF);
        }
        s_home_touch.last_x = static_cast<int16_t>(p.x);
    }
}

void OnHomeReleased(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }

    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));

    lv_indev_t* indev = lv_event_get_indev(e);
    int dx = 0;
    int dy = 0;
    if (indev != nullptr) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        dx = p.x - s_home_touch.start_x;
        dy = p.y - s_home_touch.start_y;
    }

    if (s_home_touch.axis == HomeGestureAxis::Vertical) {
        // 上下滑：不做任何处理，保持图标显示。
    } else if (s_home_touch.paging && state != nullptr) {
        SnapPagerToNearestPage(state, dx);
    } else if (!s_home_touch.consumed) {
        const uint32_t elapsed = lv_tick_elaps(s_home_touch.press_tick);
        if (HomeTouchIsTapLike(dx, dy)) {
            const HomeTouchKind kind =
                elapsed < kHomeLongPressMs ? HomeTouchKind::Click
                                           : HomeTouchKind::LongPress;
            HomeTouchDispatchTapLike(kind);
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None) {
                HomeTouchHandleSwipe(state, kind);
            }
        }
    }

    // 未进入跟手翻页且未吸附动画时，若仍在当前页则恢复图标。
    if (!s_home_touch.paging && state != nullptr && state->pager != nullptr &&
        !lv_obj_is_scrolling(state->pager)) {
        const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
        const int32_t expected = PagerScrollXForPage(state, state->current_page);
        if (scroll_x == expected) {
            SetPagerSkeletonMode(state, false);
        }
    }

    s_home_touch.active = false;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.press_cell = nullptr;
    s_home_touch.app = nullptr;
}

// 递归在子树上打 LV_OBJ_FLAG_EVENT_BUBBLE，让 cell / icon 上的 PRESSED /
// PRESSING / RELEASED 都能冒泡到屏幕被上面三个 handler 接住。
void EnableHomeEventBubble(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        EnableHomeEventBubble(lv_obj_get_child(obj, i));
    }
}

void OnHomeScreenLoaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    EnableHomeEventBubble(scr);

    // 屏幕加载完成、layout 已经算好，这时再把 pager 滚到「上次停留的页」。
    // 直接在 Create() 末尾 scroll_to_x 经常因为 layout 还没算赶不上首帧。
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state != nullptr && state->pager != nullptr) {
        lv_obj_update_layout(state->pager);
        int page = s_last_home_page;
        if (page < 0 || page >= state->page_count) {
            page = 0;
        }
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, page),
                           LV_ANIM_OFF);
        HighlightDot(state, page);
    }

    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
}

void OnHomeScreenUnloaded(lv_event_t* /*e*/) {
    // 离开首页必须出栈，否则 Test→二级页时父页 UNLOAD 后栈底仍残留 home，
    // 若二级页未登记会误把前台当成首页从而进待机。
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
}

void OnScreenDeleted(lv_event_t* e) {
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    delete static_cast<PagerState*>(lv_event_get_user_data(e));
}

// ---------------------------------------------------------------------------
// 电源对话框 (由 PwrKey_Init 长按回调经 lv_async_call 调起)
//
// 长按 PWR_KEY 弹出居中模态框，里面两个大按钮：重启 / 关机。
// 点击空白处（mask 自身，不在 card 区域内）关闭对话框。
//
// 线程模型：
//   - pwr_key_handler 在 IOExpander monitor task 里检测到长按后通过
//     lv_async_call 把 ShowPowerOptionsDialog 调度到 LVGL 线程执行。
//   - 关机操作要发 PWR_KEY_PULSE 持续脉冲，单开 FreeRTOS task 完成；
//     不阻塞 LVGL 线程，也不污染 IOExpander 的 monitor。
//
// EVENT_BUBBLE 默认关闭，所以 card / 按钮上的点击不会冒泡到 mask 触发
// 误关闭；为了给后续修改做兜底，OnPwrMaskClicked 里再做一次 target 校验。
// ---------------------------------------------------------------------------
struct PowerDialogUi {
    lv_obj_t* mask = nullptr;
    lv_obj_t* card = nullptr;
};

PowerDialogUi s_pwr_dlg;

void ClosePowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        lv_obj_delete(s_pwr_dlg.mask);
    }
    s_pwr_dlg = PowerDialogUi{};
}

void OnPwrMaskClicked(lv_event_t* e);  // forward decl

lv_obj_t* s_shutdown_screen = nullptr;

void AppendShutdownProgressContent(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spin = lv_spinner_create(box);
    lv_obj_set_size(spin, 140, 140);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, I18n::T("正在关机..."));
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateShutdownScreen() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    AppendShutdownProgressContent(screen);
    return screen;
}

// 切到独立黑底关机关机页，避免在原页面半透明遮罩上显示。
void ShowShutdownScreen() {
    ClosePowerDialog();

    if (s_shutdown_screen == nullptr) {
        s_shutdown_screen = CreateShutdownScreen();
    }
    lv_screen_load(s_shutdown_screen);
}

// 关机脉冲 task：高/低各 100ms 一直发，直到硬件断电把 task 带走。
void PwrShutdownPulseTask(void* /*arg*/) {
    auto& io = IOExpander::getInstance();
    constexpr int kPulseHalfMs = 100;
    for (;;) {
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
    }
}

void BeginSystemShutdown(const char* reason) {
    static bool shutting_down = false;
    if (shutting_down) {
        return;
    }
    shutting_down = true;

    ESP_LOGW(TAG_HOME, "%s：开始 PWR_KEY_PULSE 脉冲序列", reason);
    IdlePower_Stop();
    ShowShutdownScreen();
    xTaskCreate(PwrShutdownPulseTask, "pwr_off_pulse", 2048, nullptr, 5, nullptr);
}

void OnPwrShutdownClicked(lv_event_t* /*e*/) {
    BeginSystemShutdown(I18n::T("用户选择 [关机]"));
}

void OnPwrRebootClicked(lv_event_t* /*e*/) {
    ESP_LOGW(TAG_HOME, "用户选择 [重启]：调用 Application::Reboot()");
    ClosePowerDialog();
    Application::GetInstance().Reboot();
}

void OnPwrMaskClicked(lv_event_t* e) {
    // 只处理「点在 mask 自身」的事件。card / 按钮的点击因为 EVENT_BUBBLE
    // 默认关闭根本不会冒泡到这里；这里再做一道 target 校验是为了万一以后
    // 谁动了 EVENT_BUBBLE 标志也不会误关。
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    ESP_LOGI(TAG_HOME, "点击模态框外，关闭电源对话框");
    ClosePowerDialog();
}

lv_obj_t* CreatePowerActionBtn(lv_obj_t* parent,
                               const char* icon_src,
                               const char* text,
                               lv_event_cb_t on_click) {
    constexpr int kBtnSize     = 180;
    constexpr int kBtnIconSize = 96;

    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBtnSize, kBtnSize);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* icon = lv_image_create(btn);
    lv_image_set_src(icon, icon_src);
    lv_obj_set_size(icon, kBtnIconSize, kBtnIconSize);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, 12, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void ShowPowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        return;  // 已经打开
    }
    lv_obj_t* parent = lv_screen_active();
    if (parent == nullptr) {
        return;
    }

    // ---- 全屏遮罩 ----
    // FLOATING：让 mask 脱离父屏的 flex / grid 布局。
    // 比如 gps_screen 在根对象上挂了 LV_FLEX_FLOW_COLUMN，没有这个 flag
    // 时 mask 会被父屏的 flex 接管 —— `lv_obj_set_pos(0,0)` 会被布局覆盖
    // 重排到列尾，加上 720x720 比屏幕还大就只能看到底部一截。挂了
    // FLOATING 后 mask 完全被父布局忽略，set_pos 重新生效，dialog 在任何
    // 屏幕上都能正确居中铺满。
    lv_obj_t* mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnPwrMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_pwr_dlg.mask = mask;

    // ---- 中心卡片 ----
    constexpr int kCardW = 480;
    constexpr int kCardH = 360;
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // card 必须 clickable —— 这样点中 card 范围内的事件被 card 自己消费，
    // 不会落到 mask 触发关闭逻辑。
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_dlg.card = card;

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("电源选项"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // ---- 按钮行 ----
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 32, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    CreatePowerActionBtn(row, "A:ic_s_home_reboot.spng", I18n::T("重启"),
                         OnPwrRebootClicked);
    CreatePowerActionBtn(row, "A:ic_s_home_power.spng", I18n::T("关机"),
                         OnPwrShutdownClicked);

    // ---- 底部提示：硬件强制关机说明 ----
    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, I18n::T("长按关机键 5 秒可强制关机"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreatePage(lv_obj_t* pager, int page_index, int total_apps) {
    lv_obj_t* page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelSize, kPagerHeight);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page, kPagePadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page, kPagePadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, kGridColGap, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, kGridRowGap, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // Fixed 3x3 grid -- each app sits in its natural (col, row) slot so an
    // under-filled page (e.g. a single app on page 2) anchors top-left
    // instead of getting visually centered by a flex space-distribute.
    lv_obj_set_grid_dsc_array(page, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);

    const int start = page_index * kAppsPerPage;
    for (int i = 0; i < kAppsPerPage; ++i) {
        const int idx = start + i;
        if (idx >= total_apps)
            break;
        const AppEntry& app = kApps[idx];
        if (app.icon_suffix == nullptr)
            continue;
        lv_obj_t* cell = CreateAppCell(page, app, idx);
        const int col = i % kPageCols;
        const int row = i / kPageCols;
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    }
    return page;
}

void CreateIndicator(lv_obj_t* screen, PagerState* state) {
    // The pill-shaped capsule under the dots gives the indicator enough
    // contrast against any wallpaper / page colour without competing for
    // attention.  It only shows when there are 2+ pages.
    lv_obj_t* indicator = lv_obj_create(screen);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -kIndicatorYOffset);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(kIndicatorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_40, LV_PART_MAIN);  // ~40% black
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(indicator, kIndicatorPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(indicator, kIndicatorPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator, kDotGap, LV_PART_MAIN);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    // The indicator is purely decorative -- touch events fall through to
    // the pager underneath so the user can grab it to swipe pages.
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    state->indicator = indicator;

    for (int i = 0; i < state->page_count; ++i) {
        lv_obj_t* dot = lv_obj_create(indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        state->dots[i] = dot;
    }

    HighlightDot(state, 0);
}

// ---------------------------------------------------------------------------
// Ambient OS home
//
// The launcher is still available behind TOOLS, but the default surface is
// now context-first: LIVE shows the current device state, MEMORY holds the
// recent conversational event stream, and ASK turns the core into a large
// push target.  No synthetic memories are shown; the timeline is populated by
// actual Display::SetChatMessage calls routed through LVAdapterDisplay.
// ---------------------------------------------------------------------------

enum class AmbientPage : uint8_t { Live = 0, Memory = 1, Ask = 2 };

struct AmbientEvent {
    std::string time;
    std::string role;
    std::string text;
};

struct AmbientUiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* pages[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* nav_buttons[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* nav_labels[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* nav_markers[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* pulse = nullptr;
    lv_obj_t* core = nullptr;
    lv_obj_t* state_label = nullptr;
    lv_obj_t* sensor_label = nullptr;
    lv_obj_t* context_label = nullptr;
    lv_obj_t* context_meta = nullptr;
    lv_obj_t* memory_list = nullptr;
    lv_obj_t* ask_prompt = nullptr;
    lv_obj_t* tools_overlay = nullptr;
    lv_timer_t* state_timer = nullptr;
    AmbientPage page = AmbientPage::Live;
    DeviceState last_device_state = kDeviceStateUnknown;
    bool active = false;
};

AmbientUiState s_ambient_ui;
std::deque<AmbientEvent> s_ambient_events;
std::string s_ambient_status = "Ready";

constexpr uint32_t kAmbientBgTop = 0x07100F;
constexpr uint32_t kAmbientBgBottom = 0x05070B;
constexpr uint32_t kAmbientPanel = 0x101817;
constexpr uint32_t kAmbientPanelAlt = 0x0C1214;
constexpr uint32_t kAmbientLine = 0x243330;
constexpr uint32_t kAmbientText = 0xF2F7F5;
constexpr uint32_t kAmbientMuted = 0x8FA29D;
constexpr uint32_t kAmbientAccent = 0x45E6AD;
constexpr uint32_t kAmbientAccentDim = 0x173D32;
constexpr int kAmbientTop = kStatusBarHeight;
constexpr int kAmbientNavH = 86;
constexpr int kAmbientPageH = kPanelSize - kAmbientTop - kAmbientNavH;

lv_obj_t* AmbientLabel(lv_obj_t* parent, const char* text,
                       const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void StyleAmbientButton(lv_obj_t* button, uint32_t bg = kAmbientPanel) {
    screen_strip_obj_chrome(button);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(kAmbientLine), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(
        button, 248,
        static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
}

void SetAmbientPage(AmbientPage page);

void RefreshAmbientContext() {
    if (s_ambient_ui.context_label == nullptr || s_ambient_ui.context_meta == nullptr) {
        return;
    }
    if (!s_ambient_events.empty()) {
        const AmbientEvent& event = s_ambient_events.front();
        lv_label_set_text(s_ambient_ui.context_label, event.text.c_str());
        std::string meta = event.time + "  ·  " +
                           (event.role == "user" ? "HEARD" : "UNDERSTOOD");
        lv_label_set_text(s_ambient_ui.context_meta, meta.c_str());
    } else {
        lv_label_set_text(s_ambient_ui.context_label,
                          "Nothing important yet.\nTap the core when you need me.");
        lv_label_set_text(s_ambient_ui.context_meta, "CONTEXT IS BUILDING");
    }
}

void RefreshAmbientMemory() {
    if (s_ambient_ui.memory_list == nullptr) {
        return;
    }
    lv_obj_clean(s_ambient_ui.memory_list);
    if (s_ambient_events.empty()) {
        lv_obj_t* empty = AmbientLabel(s_ambient_ui.memory_list,
                                       "No memories in this session yet.\n\n"
                                       "Conversations will appear here as a quiet event stream.",
                                       &font_puhui_30_4, kAmbientMuted);
        lv_obj_set_width(empty, 600);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 120);
        return;
    }

    int y = 0;
    for (const auto& event : s_ambient_events) {
        lv_obj_t* row = lv_obj_create(s_ambient_ui.memory_list);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, 630, 94);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_color(row, lv_color_hex(kAmbientPanel), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(kAmbientLine), LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dot = lv_obj_create(row);
        screen_strip_obj_chrome(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_pos(dot, 20, 24);

        std::string heading = event.time + "   " +
                              (event.role == "user" ? "HEARD" : "REMEMBERED");
        lv_obj_t* meta = AmbientLabel(row, heading.c_str(), &font_puhui_20_4,
                                      kAmbientAccent);
        lv_obj_set_pos(meta, 48, 13);

        lv_obj_t* body = AmbientLabel(row, event.text.c_str(), &font_puhui_20_4,
                                      kAmbientText);
        lv_obj_set_width(body, 555);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(body, 48, 49);
        y += 106;
    }
}

void PulseAnimExec(void* var, int32_t value) {
    lv_obj_t* pulse = static_cast<lv_obj_t*>(var);
    if (pulse == nullptr) {
        return;
    }
    lv_obj_set_style_transform_scale(pulse, value, LV_PART_MAIN);
    const int32_t fade = 190 - (value - 256) * 2;
    lv_obj_set_style_border_opa(
        pulse, static_cast<lv_opa_t>(fade < 24 ? 24 : fade), LV_PART_MAIN);
}

void StartAmbientPulse(lv_obj_t* pulse) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, pulse);
    lv_anim_set_values(&anim, 256, 330);
    lv_anim_set_duration(&anim, 1800);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, PulseAnimExec);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

void RefreshAmbientState() {
    if (s_ambient_ui.state_label == nullptr || s_ambient_ui.sensor_label == nullptr) {
        return;
    }
    auto& app = Application::GetInstance();
    const DeviceState state = app.GetDeviceState();
    const char* title = "AWARE";
    const char* sensors = app.IsBackgroundRecording()
                              ? "Seeing  ·  Hearing  ·  Context building"
                              : "Camera ready  ·  Microphone ready  ·  Location ready";
    uint32_t color = kAmbientAccent;
    switch (state) {
        case kDeviceStateConnecting:
            title = "UNDERSTANDING";
            sensors = "Connecting context to the agent...";
            color = 0xF6C85F;
            break;
        case kDeviceStateListening:
            title = "LISTENING";
            sensors = "Conversation detected  ·  Speak naturally";
            color = 0x61E8FF;
            break;
        case kDeviceStateSpeaking:
            title = "RESPONDING";
            sensors = "Turning context into an answer";
            color = 0xA78BFA;
            break;
        case kDeviceStateWifiConfiguring:
            title = "OFFLINE";
            sensors = "Connect a network to continue";
            color = 0xF6C85F;
            break;
        case kDeviceStateFatalError:
            title = "NEEDS ATTENTION";
            sensors = "Open tools to inspect the device";
            color = 0xFB7185;
            break;
        default:
            break;
    }
    lv_label_set_text(s_ambient_ui.state_label, title);
    lv_obj_set_style_text_color(s_ambient_ui.state_label, lv_color_hex(color),
                                LV_PART_MAIN);
    lv_label_set_text(s_ambient_ui.sensor_label, sensors);
    if (s_ambient_ui.core != nullptr) {
        lv_obj_set_style_bg_color(s_ambient_ui.core, lv_color_hex(color), LV_PART_MAIN);
    }
    if (s_ambient_ui.pulse != nullptr) {
        lv_obj_set_style_border_color(s_ambient_ui.pulse, lv_color_hex(color),
                                      LV_PART_MAIN);
    }
    s_ambient_ui.last_device_state = state;
}

void OnAmbientStateTimer(lv_timer_t* /*timer*/) {
    RefreshAmbientState();
}

void OnAmbientCoreClicked(lv_event_t* /*event*/) {
    ResetHomeIdleTimer();
    Application::GetInstance().ToggleChatState();
}

void OnAmbientContextClicked(lv_event_t* /*event*/) {
    SetAmbientPage(AmbientPage::Memory);
}

void OnAmbientNavClicked(lv_event_t* event) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (index >= 0 && index < 3) {
        SetAmbientPage(static_cast<AmbientPage>(index));
    }
}

void OnAmbientSuggestionClicked(lv_event_t* event) {
    const char* prompt = static_cast<const char*>(lv_event_get_user_data(event));
    if (s_ambient_ui.ask_prompt != nullptr && prompt != nullptr) {
        std::string text = "Say:  \"";
        text += prompt;
        text += "\"";
        lv_label_set_text(s_ambient_ui.ask_prompt, text.c_str());
    }
    ResetHomeIdleTimer();
    Application::GetInstance().ToggleChatState();
}

void SetAmbientPage(AmbientPage page) {
    const int active = static_cast<int>(page);
    s_ambient_ui.page = page;
    for (int i = 0; i < 3; ++i) {
        if (s_ambient_ui.pages[i] != nullptr) {
            if (i == active) {
                lv_obj_remove_flag(s_ambient_ui.pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_ambient_ui.pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_ambient_ui.nav_labels[i] != nullptr) {
            lv_obj_set_style_text_color(
                s_ambient_ui.nav_labels[i],
                lv_color_hex(i == active ? kAmbientText : kAmbientMuted), LV_PART_MAIN);
        }
        if (s_ambient_ui.nav_markers[i] != nullptr) {
            lv_obj_set_style_bg_opa(s_ambient_ui.nav_markers[i],
                                    i == active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                    LV_PART_MAIN);
        }
    }
    if (page == AmbientPage::Memory) {
        RefreshAmbientMemory();
    }
    ResetHomeIdleTimer();
}

void OnAmbientAppClicked(lv_event_t* event) {
    auto* app = static_cast<AppEntry*>(lv_event_get_user_data(event));
    if (app != nullptr && app->launch != nullptr) {
        ResetHomeIdleTimer();
        app->launch(app->lifecycle_cb);
    }
}

void OnAmbientToolsClose(lv_event_t* /*event*/) {
    if (s_ambient_ui.tools_overlay != nullptr) {
        lv_obj_delete(s_ambient_ui.tools_overlay);
        s_ambient_ui.tools_overlay = nullptr;
    }
}

void OnAmbientToolsClicked(lv_event_t* /*event*/) {
    if (s_ambient_ui.screen == nullptr || s_ambient_ui.tools_overlay != nullptr) {
        return;
    }
    ResetHomeIdleTimer();
    lv_obj_t* overlay = lv_obj_create(s_ambient_ui.screen);
    s_ambient_ui.tools_overlay = overlay;
    screen_strip_obj_chrome(overlay);
    lv_obj_set_size(overlay, kPanelSize, kPanelSize - kStatusBarHeight);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(kAmbientBgBottom), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(overlay, lv_color_hex(0x10231E), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(overlay, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);

    lv_obj_t* title = AmbientLabel(overlay, "TOOLS", &font_puhui_30_4, kAmbientText);
    lv_obj_set_pos(title, 32, 22);
    lv_obj_t* subtitle = AmbientLabel(overlay, "All device capabilities",
                                      &font_puhui_20_4, kAmbientMuted);
    lv_obj_set_pos(subtitle, 32, 58);

    lv_obj_t* close = lv_button_create(overlay);
    StyleAmbientButton(close, kAmbientPanel);
    lv_obj_set_size(close, 72, 54);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -24, 20);
    lv_obj_add_event_cb(close, OnAmbientToolsClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* close_label = AmbientLabel(close, "×", &font_puhui_30_4, kAmbientText);
    lv_obj_center(close_label);

    lv_obj_t* list = lv_obj_create(overlay);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, 680, 550);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_hor(list, 25, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, 28, LV_PART_MAIN);

    constexpr int cols = 3;
    constexpr int gap_x = 55;
    constexpr int gap_y = 26;
    for (int i = 0; i < kTotalApps; ++i) {
        lv_obj_t* cell = CreateAppCell(list, kApps[i], i);
        const int col = i % cols;
        const int row = i / cols;
        lv_obj_set_pos(cell, col * (kCellWidth + gap_x), row * (kCellHeight + gap_y));
        lv_obj_add_event_cb(cell, OnAmbientAppClicked, LV_EVENT_CLICKED,
                            const_cast<AppEntry*>(&kApps[i]));
    }
}

lv_obj_t* CreateAmbientPage(lv_obj_t* screen) {
    lv_obj_t* page = lv_obj_create(screen);
    screen_strip_obj_chrome(page);
    lv_obj_set_size(page, kPanelSize, kAmbientPageH);
    lv_obj_set_pos(page, 0, kAmbientTop);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

void CreateAmbientLivePage(lv_obj_t* screen) {
    lv_obj_t* page = CreateAmbientPage(screen);
    s_ambient_ui.pages[0] = page;

    lv_obj_t* live_dot = lv_obj_create(page);
    screen_strip_obj_chrome(live_dot);
    lv_obj_set_size(live_dot, 10, 10);
    lv_obj_set_style_radius(live_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(live_dot, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(live_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(live_dot, 28, 24);
    lv_obj_t* live = AmbientLabel(page, "LIVE", &font_puhui_20_4, kAmbientText);
    lv_obj_set_pos(live, 48, 15);
    lv_obj_t* privacy = AmbientLabel(page, "ON-DEVICE  ·  PRIVATE",
                                     &font_puhui_20_4, kAmbientMuted);
    lv_obj_align(privacy, LV_ALIGN_TOP_RIGHT, -28, 15);

    lv_obj_t* pulse = lv_obj_create(page);
    s_ambient_ui.pulse = pulse;
    screen_strip_obj_chrome(pulse);
    lv_obj_set_size(pulse, 150, 150);
    lv_obj_set_style_radius(pulse, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pulse, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(pulse, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(pulse, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
    lv_obj_set_style_border_opa(pulse, LV_OPA_60, LV_PART_MAIN);
    lv_obj_align(pulse, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_remove_flag(pulse, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* core_hit = lv_button_create(page);
    screen_strip_obj_chrome(core_hit);
    lv_obj_set_size(core_hit, 190, 190);
    lv_obj_align(core_hit, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(core_hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(core_hit, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(
        core_hit, 246,
        static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(core_hit, OnAmbientCoreClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* core = lv_obj_create(core_hit);
    s_ambient_ui.core = core;
    screen_strip_obj_chrome(core);
    lv_obj_set_size(core, 76, 76);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(core, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(core, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(core, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(core, 32, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(core, LV_OPA_30, LV_PART_MAIN);
    lv_obj_center(core);
    lv_obj_t* pupil = lv_obj_create(core);
    screen_strip_obj_chrome(pupil);
    lv_obj_set_size(pupil, 28, 28);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pupil, lv_color_hex(kAmbientBgBottom), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(pupil);
    screen_make_input_passive(core_hit);
    lv_obj_add_flag(core_hit, LV_OBJ_FLAG_CLICKABLE);

    s_ambient_ui.state_label = AmbientLabel(page, "AWARE", &font_puhui_30_4,
                                            kAmbientAccent);
    lv_obj_align(s_ambient_ui.state_label, LV_ALIGN_TOP_MID, 0, 242);
    s_ambient_ui.sensor_label = AmbientLabel(
        page, "Camera ready  ·  Microphone ready  ·  Location ready",
        &font_puhui_20_4, kAmbientMuted);
    lv_obj_align(s_ambient_ui.sensor_label, LV_ALIGN_TOP_MID, 0, 281);

    lv_obj_t* card = lv_button_create(page);
    StyleAmbientButton(card);
    lv_obj_set_size(card, 664, 176);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_event_cb(card, OnAmbientContextClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* card_title = AmbientLabel(card, "✦  CURRENT CONTEXT",
                                        &font_puhui_20_4, kAmbientAccent);
    lv_obj_set_pos(card_title, 22, 16);
    s_ambient_ui.context_label = AmbientLabel(
        card, "Nothing important yet.\nTap the core when you need me.",
        &font_puhui_30_4, kAmbientText);
    lv_obj_set_width(s_ambient_ui.context_label, 610);
    lv_label_set_long_mode(s_ambient_ui.context_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ambient_ui.context_label, 22, 52);
    s_ambient_ui.context_meta = AmbientLabel(card, "CONTEXT IS BUILDING",
                                             &font_puhui_20_4, kAmbientMuted);
    lv_obj_align(s_ambient_ui.context_meta, LV_ALIGN_BOTTOM_LEFT, 22, -14);
}

void CreateAmbientMemoryPage(lv_obj_t* screen) {
    lv_obj_t* page = CreateAmbientPage(screen);
    s_ambient_ui.pages[1] = page;
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* title = AmbientLabel(page, "MEMORY", &font_puhui_30_4, kAmbientText);
    lv_obj_set_pos(title, 28, 18);
    lv_obj_t* subtitle = AmbientLabel(page, "A living stream of what mattered",
                                      &font_puhui_20_4, kAmbientMuted);
    lv_obj_set_pos(subtitle, 28, 56);
    lv_obj_t* list = lv_obj_create(page);
    s_ambient_ui.memory_list = list;
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, 664, kAmbientPageH - 100);
    lv_obj_set_pos(list, 28, 92);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    RefreshAmbientMemory();
}

void CreateAmbientAskPage(lv_obj_t* screen) {
    lv_obj_t* page = CreateAmbientPage(screen);
    s_ambient_ui.pages[2] = page;
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* title = AmbientLabel(page, "ASK MEMORY", &font_puhui_30_4,
                                   kAmbientText);
    lv_obj_set_pos(title, 28, 18);
    lv_obj_t* subtitle = AmbientLabel(page, "Ask your world, not a blank chatbot",
                                      &font_puhui_20_4, kAmbientMuted);
    lv_obj_set_pos(subtitle, 28, 56);

    lv_obj_t* prompt_card = lv_obj_create(page);
    screen_strip_obj_chrome(prompt_card);
    lv_obj_set_size(prompt_card, 664, 112);
    lv_obj_set_pos(prompt_card, 28, 98);
    lv_obj_set_style_bg_color(prompt_card, lv_color_hex(kAmbientAccentDim), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(prompt_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(prompt_card, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(prompt_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(prompt_card, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
    s_ambient_ui.ask_prompt = AmbientLabel(prompt_card,
                                           "What do you remember about today?",
                                           &font_puhui_30_4, kAmbientText);
    lv_obj_set_width(s_ambient_ui.ask_prompt, 610);
    lv_label_set_long_mode(s_ambient_ui.ask_prompt, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_ambient_ui.ask_prompt);

    const char* prompts[] = {
        "What happened today?",
        "What did I promise?",
        "What should I follow up?",
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* chip = lv_button_create(page);
        StyleAmbientButton(chip, kAmbientPanelAlt);
        lv_obj_set_size(chip, 600, 58);
        lv_obj_set_pos(chip, 60, 232 + i * 70);
        lv_obj_add_event_cb(chip, OnAmbientSuggestionClicked, LV_EVENT_CLICKED,
                            const_cast<char*>(prompts[i]));
        lv_obj_t* label = AmbientLabel(chip, prompts[i], &font_puhui_20_4,
                                       kAmbientText);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t* arrow = AmbientLabel(chip, "›", &font_puhui_30_4, kAmbientAccent);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -18, -2);
    }

    lv_obj_t* ask = lv_button_create(page);
    StyleAmbientButton(ask, kAmbientAccent);
    lv_obj_set_size(ask, 300, 68);
    lv_obj_align(ask, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_border_width(ask, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(ask, OnAmbientCoreClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* ask_label = AmbientLabel(ask, "TAP TO SPEAK", &font_puhui_20_4,
                                       kAmbientBgBottom);
    lv_obj_center(ask_label);
}

void CreateAmbientNav(lv_obj_t* screen) {
    lv_obj_t* nav = lv_obj_create(screen);
    screen_strip_obj_chrome(nav);
    lv_obj_set_size(nav, kPanelSize, kAmbientNavH);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x090D0F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(nav, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(nav, lv_color_hex(kAmbientLine), LV_PART_MAIN);

    const char* names[] = {"LIVE", "MEMORY", "ASK"};
    constexpr int nav_w = 180;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* button = lv_button_create(nav);
        s_ambient_ui.nav_buttons[i] = button;
        screen_strip_obj_chrome(button);
        lv_obj_set_size(button, nav_w, kAmbientNavH);
        lv_obj_set_pos(button, i * nav_w, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_event_cb(button, OnAmbientNavClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* label = AmbientLabel(button, names[i], &font_puhui_20_4,
                                       i == 0 ? kAmbientText : kAmbientMuted);
        s_ambient_ui.nav_labels[i] = label;
        lv_obj_center(label);
        lv_obj_t* marker = lv_obj_create(button);
        s_ambient_ui.nav_markers[i] = marker;
        screen_strip_obj_chrome(marker);
        lv_obj_set_size(marker, 36, 3);
        lv_obj_align(marker, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(marker, lv_color_hex(kAmbientAccent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(marker, i == 0 ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
    }

    lv_obj_t* tools = lv_button_create(nav);
    screen_strip_obj_chrome(tools);
    lv_obj_set_size(tools, kPanelSize - nav_w * 3, kAmbientNavH);
    lv_obj_set_pos(tools, nav_w * 3, 0);
    lv_obj_set_style_bg_color(tools, lv_color_hex(kAmbientPanel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tools, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(tools, OnAmbientToolsClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* tools_label = AmbientLabel(tools, "TOOLS", &font_puhui_20_4,
                                         kAmbientMuted);
    lv_obj_center(tools_label);
}

void OnAmbientScreenLoaded(lv_event_t* /*event*/) {
    s_ambient_ui.active = true;
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
    RefreshAmbientState();
    RefreshAmbientContext();
}

void OnAmbientScreenUnloaded(lv_event_t* /*event*/) {
    s_ambient_ui.active = false;
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
}

void OnAmbientScreenDeleted(lv_event_t* /*event*/) {
    if (s_ambient_ui.state_timer != nullptr) {
        lv_timer_delete(s_ambient_ui.state_timer);
    }
    if (s_ambient_ui.pulse != nullptr) {
        lv_anim_delete(s_ambient_ui.pulse, nullptr);
    }
    StopHomeIdleTimer();
    s_ambient_ui = AmbientUiState{};
}

}  // namespace

void HomeScreen::ShowPowerOptionsDialog() { ShowPowerDialog(); }

lv_obj_t* HomeScreen::Create() {
    // Ambient OS 以世界状态为首页；完整 app launcher 保留在 TOOLS 抽屉。
    EnsureIconPathsBuilt();

    lv_obj_t* screen = lv_obj_create(NULL);
    s_ambient_ui = AmbientUiState{};
    s_ambient_ui.screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(kAmbientBgTop), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(kAmbientBgBottom), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    auto* status = new HomeStatusState{};
    CreateStatusBar(screen, status);

    CreateAmbientLivePage(screen);
    CreateAmbientMemoryPage(screen);
    CreateAmbientAskPage(screen);
    CreateAmbientNav(screen);
    SetAmbientPage(AmbientPage::Live);
    RefreshAmbientContext();
    RefreshAmbientState();
    StartAmbientPulse(s_ambient_ui.pulse);
    s_ambient_ui.state_timer = lv_timer_create(OnAmbientStateTimer, 350, nullptr);

    lv_obj_add_event_cb(screen, OnAmbientScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(screen, OnAmbientScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(screen, OnAmbientScreenDeleted, LV_EVENT_DELETE, nullptr);

    return screen;
}

bool HomeScreen::IsActive() { return s_ambient_ui.active; }

void HomeScreen::SetAmbientStatus(const char* status) {
    if (status == nullptr || status[0] == '\0') {
        return;
    }
    s_ambient_status = status;
    if (s_ambient_ui.active && s_ambient_events.empty() &&
        s_ambient_ui.context_label != nullptr) {
        lv_label_set_text(s_ambient_ui.context_label, status);
    }
    RefreshAmbientState();
}

void HomeScreen::AddAmbientEvent(const char* role, const char* content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }
    AmbientEvent event;
    time_t now = time(nullptr);
    struct tm local = {};
    char time_buf[8] = "NOW";
    if (localtime_r(&now, &local) != nullptr && local.tm_year >= 2025 - 1900) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", &local);
    }
    event.time = time_buf;
    event.role = role;
    event.text.assign(content, std::min<size_t>(std::strlen(content), 180));
    s_ambient_events.push_front(std::move(event));
    while (s_ambient_events.size() > 8) {
        s_ambient_events.pop_back();
    }
    if (s_ambient_ui.active) {
        RefreshAmbientContext();
        if (s_ambient_ui.page == AmbientPage::Memory) {
            RefreshAmbientMemory();
        }
    }
}

void OnRefreshStatusBarAsync(void* /*user_data*/) {
    if (s_home_status != nullptr) {
        UpdateHomeStatusBar(s_home_status);
    }
}

void HomeScreen::ResetToFirstPage() {
    s_last_home_page = 0;
}

void HomeScreen::RefreshStatusBar() {
    // Application::CheckNewVersion / ShowActivationCode 跑在 app_main
    // 线程，不能直接碰 LVGL；切到 LVGL 线程再刷新，避免与 adapter 争用锁
    // 导致 lv_obj_invalidate 在 Core0 上死循环占满 CPU。
    lv_async_call(OnRefreshStatusBarAsync, nullptr);
}

int HomeScreen::GetIdleShutdownMinutes() {
    return IdlePower_GetShutdownMinutes();
}

void HomeScreen::SetIdleShutdownMinutes(int minutes) {
    IdlePower_SetShutdownMinutes(minutes);
    IdlePower_NotifyActivity();
}

int HomeScreen::GetIdleStandbyMinutes() {
    return IdlePower_GetStandbyMinutes();
}

void HomeScreen::SetIdleStandbyMinutes(int minutes) {
    IdlePower_SetStandbyMinutes(minutes);
    IdlePower_NotifyActivity();
}

void HomeScreen::RequestSystemShutdown(const char* reason) {
    BeginSystemShutdown(reason != nullptr ? reason : I18n::T("系统关机"));
}
