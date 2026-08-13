#pragma once

#include <esp_err.h>

// Installs the firmware-owned ESP-Claw skill documents under builtin_root.
// Documents are refreshed at boot so their instructions always match the
// running firmware. User-created skills live in a separate writable root.
esp_err_t InstallEspClawBuiltinSkills(const char* builtin_root);
