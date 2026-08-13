#include "esp_claw_game_ui.h"

#include "display/screen/digital_people_mode.h"
#include "display/touch_feed.h"

#include <esp_log.h>
#include <esp_lv_adapter.h>
#include <lvgl.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
extern const lv_font_t BUILTIN_TEXT_FONT;
}

#include <cap_lua.h>

#include <algorithm>
#include <cstdint>

namespace {

constexpr const char* TAG = "EspClawGameUi";

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_canvas = nullptr;
lua_State* s_owner = nullptr;
lua_State* s_frame_owner = nullptr;

uint32_t ParseColor(lua_State* state, int index) {
    const lua_Integer value = luaL_checkinteger(state, index);
    return static_cast<uint32_t>(value) & 0x00ffffffU;
}

bool LockUi() {
    return esp_lv_adapter_lock(-1) == ESP_OK;
}

void UnlockUi() {
    esp_lv_adapter_unlock();
}

bool IsOwnedAndValid(lua_State* state) {
    return s_owner == state && s_screen != nullptr && s_canvas != nullptr &&
           lv_obj_is_valid(s_screen) && lv_obj_is_valid(s_canvas);
}

void DestroyDrawBufferOnDelete(lv_event_t* event) {
    auto* draw_buffer =
        static_cast<lv_draw_buf_t*>(lv_event_get_user_data(event));
    if (draw_buffer != nullptr) {
        lv_draw_buf_destroy(draw_buffer);
    }
}

int RequireCanvas(lua_State* state) {
    if (!IsOwnedAndValid(state)) {
        return luaL_error(state,
                          "metalio_ui is not open; call metalio_ui.open() first");
    }
    return 0;
}

int Open(lua_State* state) {
    if (s_owner != nullptr && s_owner != state) {
        return luaL_error(state, "another game already owns the display");
    }
    if (IsOwnedAndValid(state)) {
        lua_pushinteger(state,
                        lv_display_get_horizontal_resolution(lv_display_get_default()));
        lua_pushinteger(state,
                        lv_display_get_vertical_resolution(lv_display_get_default()));
        return 2;
    }
    if (!LockUi()) {
        return luaL_error(state, "failed to lock the Metalio display");
    }

    lv_display_t* display = lv_display_get_default();
    const int32_t width = lv_display_get_horizontal_resolution(display);
    const int32_t height = lv_display_get_vertical_resolution(display);
    lv_draw_buf_t* draw_buffer =
        lv_draw_buf_create(width, height, LV_COLOR_FORMAT_RGB565, 0);
    if (draw_buffer == nullptr) {
        UnlockUi();
        return luaL_error(state, "failed to allocate the game canvas");
    }

    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    lv_obj_t* canvas = lv_canvas_create(screen);
    lv_canvas_set_draw_buf(canvas, draw_buffer);
    lv_obj_set_pos(canvas, 0, 0);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_event_cb(canvas, DestroyDrawBufferOnDelete, LV_EVENT_DELETE,
                        draw_buffer);

    lv_obj_t* old_screen = lv_screen_active();
    lv_screen_load(screen);
    if (old_screen != nullptr && old_screen != screen) {
        lv_obj_delete_async(old_screen);
    }

    s_screen = screen;
    s_canvas = canvas;
    s_owner = state;
    UnlockUi();

    ESP_LOGI(TAG, "Lua game canvas opened: %ldx%ld", static_cast<long>(width),
             static_cast<long>(height));
    lua_pushinteger(state, width);
    lua_pushinteger(state, height);
    return 2;
}

void CloseOwned(lua_State* state, bool force) {
    if (!force && s_owner != state) {
        return;
    }
    if (s_frame_owner != nullptr) {
        if (s_frame_owner == state) {
            s_frame_owner = nullptr;
            UnlockUi();
        } else {
            ESP_LOGW(TAG, "Cannot close game UI while another task draws a frame");
            return;
        }
    }
    if (s_screen == nullptr) {
        s_owner = nullptr;
        s_canvas = nullptr;
        return;
    }
    if (!LockUi()) {
        return;
    }
    s_owner = nullptr;
    s_canvas = nullptr;
    s_screen = nullptr;
    DigitalPeopleMode::ShowDigitalPeople();
    UnlockUi();
    ESP_LOGI(TAG, "Lua game canvas closed");
}

int Close(lua_State* state) {
    CloseOwned(state, false);
    return 0;
}

int BeginFrame(lua_State* state) {
    RequireCanvas(state);
    if (s_frame_owner != nullptr) {
        return luaL_error(state, "a Metalio frame is already being drawn");
    }
    const uint32_t color =
        lua_gettop(state) >= 1 ? ParseColor(state, 1) : 0x000000U;
    if (!LockUi()) {
        return luaL_error(state, "failed to lock the Metalio display");
    }
    if (!IsOwnedAndValid(state)) {
        UnlockUi();
        return luaL_error(state, "game canvas was closed");
    }
    s_frame_owner = state;
    lv_canvas_fill_bg(s_canvas, lv_color_hex(color), LV_OPA_COVER);
    return 0;
}

int EndFrame(lua_State* state) {
    if (s_frame_owner != state) {
        return luaL_error(state, "begin_frame() was not called");
    }
    if (s_canvas != nullptr && lv_obj_is_valid(s_canvas)) {
        lv_obj_invalidate(s_canvas);
    }
    s_frame_owner = nullptr;
    UnlockUi();
    return 0;
}

bool LockForDraw(lua_State* state, bool* unlock_after) {
    *unlock_after = s_frame_owner != state;
    if (*unlock_after && !LockUi()) {
        return false;
    }
    if (!IsOwnedAndValid(state)) {
        if (*unlock_after) {
            UnlockUi();
        }
        return false;
    }
    return true;
}

void FinishDraw(bool unlock_after) {
    if (unlock_after) {
        if (s_canvas != nullptr && lv_obj_is_valid(s_canvas)) {
            lv_obj_invalidate(s_canvas);
        }
        UnlockUi();
    }
}

int Rect(lua_State* state) {
    RequireCanvas(state);
    const int32_t x = static_cast<int32_t>(luaL_checkinteger(state, 1));
    const int32_t y = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const int32_t width =
        std::max<int32_t>(1, static_cast<int32_t>(luaL_checkinteger(state, 3)));
    const int32_t height =
        std::max<int32_t>(1, static_cast<int32_t>(luaL_checkinteger(state, 4)));
    const uint32_t color = ParseColor(state, 5);
    const bool filled = lua_gettop(state) < 6 || lua_toboolean(state, 6);
    const int32_t radius = lua_gettop(state) >= 7
                               ? std::max<int32_t>(0, lua_tointeger(state, 7))
                               : 0;
    bool unlock_after = false;
    if (!LockForDraw(state, &unlock_after)) {
        return luaL_error(state, "game canvas was closed");
    }
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.radius = radius;
    descriptor.bg_color = lv_color_hex(color);
    descriptor.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
    descriptor.border_color = lv_color_hex(color);
    descriptor.border_opa = LV_OPA_COVER;
    descriptor.border_width = filled ? 0 : 2;
    const lv_area_t area = {x, y, x + width - 1, y + height - 1};
    lv_draw_rect(&layer, &descriptor, &area);
    lv_canvas_finish_layer(s_canvas, &layer);
    FinishDraw(unlock_after);
    return 0;
}

int Circle(lua_State* state) {
    RequireCanvas(state);
    const int32_t center_x = static_cast<int32_t>(luaL_checkinteger(state, 1));
    const int32_t center_y = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const int32_t radius =
        std::max<int32_t>(1, static_cast<int32_t>(luaL_checkinteger(state, 3)));
    const uint32_t color = ParseColor(state, 4);
    const bool filled = lua_gettop(state) < 5 || lua_toboolean(state, 5);
    bool unlock_after = false;
    if (!LockForDraw(state, &unlock_after)) {
        return luaL_error(state, "game canvas was closed");
    }
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.radius = LV_RADIUS_CIRCLE;
    descriptor.bg_color = lv_color_hex(color);
    descriptor.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
    descriptor.border_color = lv_color_hex(color);
    descriptor.border_opa = LV_OPA_COVER;
    descriptor.border_width = filled ? 0 : 2;
    const lv_area_t area = {center_x - radius, center_y - radius,
                            center_x + radius, center_y + radius};
    lv_draw_rect(&layer, &descriptor, &area);
    lv_canvas_finish_layer(s_canvas, &layer);
    FinishDraw(unlock_after);
    return 0;
}

int Line(lua_State* state) {
    RequireCanvas(state);
    const int32_t x1 = static_cast<int32_t>(luaL_checkinteger(state, 1));
    const int32_t y1 = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const int32_t x2 = static_cast<int32_t>(luaL_checkinteger(state, 3));
    const int32_t y2 = static_cast<int32_t>(luaL_checkinteger(state, 4));
    const uint32_t color = ParseColor(state, 5);
    const int32_t width = lua_gettop(state) >= 6
                              ? std::max<int32_t>(1, lua_tointeger(state, 6))
                              : 2;
    bool unlock_after = false;
    if (!LockForDraw(state, &unlock_after)) {
        return luaL_error(state, "game canvas was closed");
    }
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_line_dsc_t descriptor;
    lv_draw_line_dsc_init(&descriptor);
    descriptor.color = lv_color_hex(color);
    descriptor.width = width;
    descriptor.round_start = 1;
    descriptor.round_end = 1;
    descriptor.p1 = {x1, y1};
    descriptor.p2 = {x2, y2};
    lv_draw_line(&layer, &descriptor);
    lv_canvas_finish_layer(s_canvas, &layer);
    FinishDraw(unlock_after);
    return 0;
}

int Text(lua_State* state) {
    RequireCanvas(state);
    const int32_t x = static_cast<int32_t>(luaL_checkinteger(state, 1));
    const int32_t y = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const char* text = luaL_checkstring(state, 3);
    const uint32_t color = ParseColor(state, 4);
    const int32_t max_width = lua_gettop(state) >= 5
                                  ? std::max<int32_t>(1, lua_tointeger(state, 5))
                                  : 700;
    bool unlock_after = false;
    if (!LockForDraw(state, &unlock_after)) {
        return luaL_error(state, "game canvas was closed");
    }
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_label_dsc_t descriptor;
    lv_draw_label_dsc_init(&descriptor);
    descriptor.color = lv_color_hex(color);
    descriptor.font = &BUILTIN_TEXT_FONT;
    descriptor.text = text;
    const lv_area_t area = {x, y, x + max_width - 1, y + 80};
    lv_draw_label(&layer, &descriptor, &area);
    lv_canvas_finish_layer(s_canvas, &layer);
    FinishDraw(unlock_after);
    return 0;
}

int Touch(lua_State* state) {
    TouchFeedSnapshot snapshot;
    if (!touch_feed_get_snapshot(&snapshot)) {
        snapshot = {};
    }
    lua_pushboolean(state, snapshot.pressed);
    lua_pushinteger(state, snapshot.x);
    lua_pushinteger(state, snapshot.y);
    return 3;
}

int Size(lua_State* state) {
    lv_display_t* display = lv_display_get_default();
    lua_pushinteger(state, lv_display_get_horizontal_resolution(display));
    lua_pushinteger(state, lv_display_get_vertical_resolution(display));
    return 2;
}

void RuntimeCleanup(lua_State* state) {
    CloseOwned(state, false);
}

int OpenModule(lua_State* state) {
    static const luaL_Reg functions[] = {
        {"open", Open},
        {"close", Close},
        {"begin_frame", BeginFrame},
        {"end_frame", EndFrame},
        {"rect", Rect},
        {"circle", Circle},
        {"line", Line},
        {"text", Text},
        {"touch", Touch},
        {"size", Size},
        {nullptr, nullptr},
    };
    luaL_newlib(state, functions);
    return 1;
}

}  // namespace

esp_err_t RegisterEspClawGameUiModule() {
    esp_err_t error = cap_lua_register_module("metalio_ui", OpenModule);
    if (error != ESP_OK) {
        return error;
    }
    return cap_lua_register_exit_cleanup(RuntimeCleanup);
}

void StopEspClawGameUi() {
    CloseOwned(nullptr, true);
}
