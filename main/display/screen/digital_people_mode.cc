#include "digital_people_mode.h"

#include "digital_people_screen/digital_people_screen.h"
#include "network_screen/network_screen.h"
#include "pwr_key_handler.h"
#include "screen_util.h"

namespace {

void DigitalPeopleLifecycle(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("digital_people", event);
    DigitalPeopleScreen::LifecycleCallback(event);
}

void NetworkLifecycle(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("network", event);
    NetworkScreen::LifecycleCallback(event);
}

void LoadAndReplace(lv_obj_t* screen) {
    if (screen == nullptr) {
        return;
    }
    lv_obj_t* old_screen = lv_screen_active();
    lv_screen_load(screen);
    if (old_screen != nullptr && old_screen != screen) {
        lv_obj_delete_async(old_screen);
    }
}

}  // namespace

void DigitalPeopleMode::ShowDigitalPeople() {
    lv_obj_t* screen = DigitalPeopleScreen::Create();
    screen_attach_lifecycle(screen, DigitalPeopleLifecycle);
    LoadAndReplace(screen);
}

void DigitalPeopleMode::ShowNetworkSettings() {
    lv_obj_t* screen = NetworkScreen::Create();
    screen_attach_lifecycle(screen, NetworkLifecycle);
    LoadAndReplace(screen);
}
