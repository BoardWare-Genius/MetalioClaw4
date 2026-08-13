#pragma once

#include <esp_err.h>

// Register the Metalio-specific Lua canvas module before cap_lua is started.
// Lua scripts load it with: local ui = require("metalio_ui")
esp_err_t RegisterEspClawGameUiModule();

// Close an active Lua-owned game screen and return to the digital-person UI.
void StopEspClawGameUi();
