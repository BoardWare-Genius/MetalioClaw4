#pragma once

#include "lvgl.h"

class HomeScreen {
public:
    // Create a fullscreen "home" page on a 720x720 panel.
    // Lays out app icons in a 3x3 grid (phone-style) with the icon name
    // shown directly below each icon. Returns the created LVGL screen
    // object (parent = NULL).
    static lv_obj_t* Create();
    static void RefreshStatusBar();
    // Ambient OS 首页状态桥：LVAdapterDisplay 在持有 LVGL 锁时调用。
    static bool IsActive();
    static void SetAmbientStatus(const char* status);
    static void AddAmbientEvent(const char* role, const char* content);
    // 下次进入主屏时从第一页开始（用于主题切换等场景）。
    static void ResetToFirstPage();

    // PWR_KEY 长按后弹出 [重启 / 关机] 对话框（须在 LVGL 线程调用）。
    static void ShowPowerOptionsDialog();
    // 软件关机（UI / 空闲策略共用）。
    static void RequestSystemShutdown(const char* reason);

    // 无操作进入待机时长；自动关机兼容接口固定为禁用。
    static int GetIdleShutdownMinutes();
    static void SetIdleShutdownMinutes(int minutes);
    static int GetIdleStandbyMinutes();
    static void SetIdleStandbyMinutes(int minutes);
};
