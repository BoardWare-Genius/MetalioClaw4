#include "lv_adapter_display.h"

#include <cstring>
#include <memory>

#include <esp_lcd_panel_io.h>
#include <esp_log.h>

#include "esp_lv_adapter.h"
#include "esp_lv_fs.h"
#include "esp_mmap_assets.h"
#include "touch_feed.h"   

#include "mmap_generate_resources.h"

#include "screen/boot_screen/boot_screen.h"
#include "screen/digital_people_mode.h"
#include "screen/chat_screen/chat_screen.h"
#include "screen/digital_people_screen/digital_people_screen.h"
#include "screen/home_screen/home_screen.h"

#include "application.h"

static const char* TAG = "LVAdapterDisplay";

namespace {

// ---------------------------------------------------------------------------
// 表情大类映射
//
// 服务器 / LLM 返回的细分表情多达 21 种，但端侧 SD 卡只准备了 6 个大类的
// .eaf 动画（crying / happy / loving / neutral / surprised / thinking）。
// 这里把细分名收敛到大类代表，再交给 DigitalPeopleScreen 拼路径加载。
// 表中没收录的（或 nullptr）一律 fallback 到 neutral，保证永远有动画播放。
// ---------------------------------------------------------------------------
struct EmoteCategoryEntry {
    const char* emote;     // 细分表情名
    const char* category;  // 所属大类（代表表情名）
};

constexpr EmoteCategoryEntry kEmoteCategoryMap[] = {
    // 开心类 -> happy
    {"happy",       "happy"},
    {"laughing",    "happy"},
    {"funny",       "happy"},
    {"silly",       "happy"},
    {"winking",     "happy"},
    {"cool",        "happy"},
    {"confident",   "happy"},
    // 爱意类 -> loving
    {"loving",      "loving"},
    {"kissy",       "loving"},
    {"delicious",   "loving"},
    // 悲伤 / 负面类 -> crying（6 大类里用 crying 这个名字而不是 sad）
    {"sad",         "crying"},
    {"crying",      "crying"},
    {"angry",       "crying"},
    // 惊讶类 -> surprised
    {"surprised",   "surprised"},
    {"shocked",     "surprised"},
    {"embarrassed", "surprised"},
    // 思考类 -> thinking
    {"thinking",    "thinking"},
    {"confused",    "thinking"},
    // 平静类 -> neutral
    {"neutral",     "neutral"},
    {"relaxed",     "neutral"},
    {"sleepy",      "neutral"},
};

// 输入任一细分表情名，返回所属大类代表名；找不到时返回 "neutral"。
// 表大小固定 < 32，O(N) 线性比较完全够用。
const char* GetEmoteCategory(const char* emote) {
    if (emote == nullptr) return "neutral";
    for (const auto& e : kEmoteCategoryMap) {
        if (std::strcmp(e.emote, emote) == 0) {
            return e.category;
        }
    }
    return "neutral";
}

}  // namespace

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height) {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    // 性能调优要点（720x720 RGB888 屏）：
    //   - enable_ppa_accel: 关闭 LVGL PPA 绘制加速。内部 partial 条带本身仍由
    //     DMA2D 搬到 panel FB；关闭的只是圆角、粒子等对象的 PPA fill/blend。
    //     否则低内存时 LVGL 的临时绘制层可能落入不可 cache-sync 的内部区域，
    //     待机充电粒子动画会反复报 esp_cache_msync invalid addr。
    //   - tear_avoid_mode = TRIPLE_PARTIAL：LCD 驱动仍提供 3 张 panel 帧缓冲
    //     做无撕裂流水线，但 LVGL 只在一块内部 SRAM 条带上绘制脏区，再复制到
    //     下一张 panel FB。这样首页动画不会以约 60 FPS 重写整张 PSRAM 帧，
    //     可显著降低与 MIPI-CSI 摄像头并行工作时的外部内存带宽峰值。
    //   - 40 行 RGB888 条带占 720×40×3 = 86400 bytes。此前 200 行条带需要
    //     432000 bytes，容易因内部 SRAM 不足而回退；40 行在吞吐和可分配性之间
    //     更稳妥。partial 模式只需要一个 draw buffer，面板的三缓冲另行管理。
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = panel_io,
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
                .hor_res = static_cast<uint16_t>(width),
                .ver_res = static_cast<uint16_t>(height),
                .buffer_height = 40,
                .use_psram = false,
                .enable_ppa_accel = false,
                .require_double_buffer = false,
            },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
    };

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
    lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
    touch_feed_init(touch_handle, 20);
    touch_feed_attach_indev(touch_indev);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    mmap_assets_handle_t assets;
    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &assets));

    esp_lv_fs_handle_t fs_handle;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = assets,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }

    // Application::GetInstance().ForceReturnToIdle();
}

void LVAdapterDisplay::SetupUI() {
    lv_obj_t* boot_scr = BootScreen::Create();
    lv_screen_load(boot_scr);

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
#if CONFIG_DIGITAL_PEOPLE_ONLY
                DigitalPeopleMode::ShowDigitalPeople();
#else
                lv_obj_t* old_scr = lv_screen_active();
                lv_obj_t* home_scr = HomeScreen::Create();
                lv_screen_load(home_scr);
                if (old_scr != NULL && old_scr != home_scr) {
                    lv_obj_delete(old_scr);
                }
#endif
                esp_lv_adapter_unlock();
            }

            lv_timer_delete(t);
        },
        2000, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

LVAdapterDisplay::~LVAdapterDisplay() = default;

void LVAdapterDisplay::SetEmotion(const char* const emotion) {
    // 1) 把细分表情名映射到 6 个大类之一（兜底 neutral）
    const char* category = GetEmoteCategory(emotion);
    ESP_LOGI(TAG, "SetEmotion: %s -> %s",
             emotion != nullptr ? emotion : "<null>", category);

    // 2) 转交：数字人屏用 6 大类；聊天屏表情模式用服务器原始情绪名
    //    （对应 S:/sdcard/system/chat/{emotion}.eaf）。这把锁和
    //    SetChatMessage 共用，确保 lv_eaf_set_src 与屏幕生命周期互斥。
    //    屏幕不在前台时 SetEmotion 只更新静态缓存，下次 Create / 切到
    //    表情模式再加载，故无需判断 IsActive()。
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    DigitalPeopleScreen::SetEmotion(category);
#if !CONFIG_DIGITAL_PEOPLE_ONLY
    ChatScreen::SetEmotion(emotion != nullptr ? emotion : "neutral");
#endif
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetChatMessage(const char* const role, const char* const content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }

    // role 归一化：
    //   user             -> 用户发言
    //   assistant/system -> 设备 / AI 回应
    const bool is_user = (std::strcmp(role, "user") == 0);
    const bool is_bot  = (std::strcmp(role, "assistant") == 0 ||
                          std::strcmp(role, "system") == 0);
    if (!is_user && !is_bot) {
        return;
    }

    // 路由策略：
    //   1) 对话事件同时进入 Ambient OS 的短事件流。
    //   2) 聊天屏在前台 -> 历史滚动气泡（双侧）。
    //   3) 数字人屏在前台 -> user 走底部气泡，bot 走 gif 左上方气泡。
    const bool dp_active   = DigitalPeopleScreen::IsActive();
#if CONFIG_DIGITAL_PEOPLE_ONLY
    if (!dp_active) {
        return;
    }
#else
    const bool chat_active = ChatScreen::IsActive();
    const bool home_active = HomeScreen::IsActive();
    if (!chat_active && !dp_active && !home_active) {
        return;
    }
#endif

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
#if !CONFIG_DIGITAL_PEOPLE_ONLY
    HomeScreen::AddAmbientEvent(role, content);
    if (chat_active) {
        ChatScreen::AddMessage(content,
                               is_user ? ChatMsgDir::Right : ChatMsgDir::Left);
    } else if (!home_active) {
#endif
        if (is_user) {
            DigitalPeopleScreen::ShowUserMessage(content);
        } else {
            DigitalPeopleScreen::ShowSystemMessage(content);
        }
#if !CONFIG_DIGITAL_PEOPLE_ONLY
    }
#endif
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetStatus(const char* const status) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    DigitalPeopleScreen::SetStatus(status);
#if !CONFIG_DIGITAL_PEOPLE_ONLY
    HomeScreen::SetAmbientStatus(status);
#endif
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {}

void LVAdapterDisplay::SetPreviewImage(const void* image) {}

void LVAdapterDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool LVAdapterDisplay::Lock(const int timeout_ms) { return true; }

void LVAdapterDisplay::Unlock() {}
