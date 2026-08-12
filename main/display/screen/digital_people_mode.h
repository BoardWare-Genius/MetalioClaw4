#pragma once

// Dedicated firmware navigation. These functions must be called from the
// LVGL task (or while holding the LVGL adapter lock).
namespace DigitalPeopleMode {

void ShowDigitalPeople();
void ShowNetworkSettings();

}  // namespace DigitalPeopleMode
