#pragma once

#include <cstdint>

// 首页 / 待机页共享的无操作计时：
//   - 进入待机：仅在首页累计空闲达到阈值时触发
//   - 自动关机：已禁用；空闲只会进入待机，不会触发软件断电
// 进入其他 App 时 Detach 停止计时；自动进入待机会保留空闲起点。

enum class IdlePowerSession : uint8_t {
    None = 0,
    Home,
    Standby,
};

void IdlePower_NotifyActivity();
void IdlePower_Attach(IdlePowerSession session, bool reset_activity);
void IdlePower_Detach(IdlePowerSession session);
void IdlePower_Stop();

// 自动待机切换前调用：保留 last_activity，避免删除首页时清掉累计时间。
void IdlePower_PrepareAutoStandby();

int IdlePower_GetStandbyMinutes();
void IdlePower_SetStandbyMinutes(int minutes);
// 兼容旧调用；始终返回 0，设置非零值会被忽略。
int IdlePower_GetShutdownMinutes();
void IdlePower_SetShutdownMinutes(int minutes);
