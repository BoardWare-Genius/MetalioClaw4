#include "esp_claw_builtin_skills.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr const char* kChatCodingSkill = R"skill(---
{
  "name": "metalio_chat_coding",
  "description": "Create, run, stop, or modify touch games, animations, and interactive Lua screens on Beaverbeaver.",
  "metadata": {
    "cap_groups": ["cap_files", "cap_lua"],
    "manage_mode": "readonly"
  }
}
---

# Metalio Chat Coding

Use this skill whenever the user asks to create, change, run, stop, or debug a
game, animation, or interactive screen on this device.

## Runtime

- Save generated Lua only below `/sdcard/esp_claw/scripts/`.
- Load `metalio_ui` with `local ui = require("metalio_ui")`.
- Load delays with `local delay = require("delay")`.
- `w,h = ui.open()` acquires the Metalio display; always call `ui.close()` on exit.
- Draw one complete frame between `ui.begin_frame(0xRRGGBB)` and `ui.end_frame()`.
- Available drawing calls are:
  - `ui.rect(x,y,w,h,0xRRGGBB,filled[,radius])`
  - `ui.circle(cx,cy,r,0xRRGGBB,filled)`
  - `ui.line(x1,y1,x2,y2,0xRRGGBB[,width])`
  - `ui.text(x,y,text,0xRRGGBB[,max_width])`
  - `pressed,x,y = ui.touch()`
- Call `delay.delay_ms(30)` or longer in every loop. Compute state before
  `begin_frame` and keep the frame lock short.

## Required flow

1. Write a complete script with `write_file`.
2. Start interactive or long-running code with `lua_run_script_async`, using
   `exclusive="display"`, `replace=true`, and `timeout_ms=0`.
3. Treat `RUNNING` as successful launch and immediately reply with a short
   confirmation. Do not poll a running game unless the user asks for diagnosis.
4. Include a visible touch quit area in every interactive program.
5. When the user asks to stop or close it, call `lua_stop_async_job` or
   `lua_stop_all_async_jobs`; do not only say that it was stopped.
6. Never use Lua `io` or `os` to escape `/sdcard/esp_claw`.

## Minimal loop

```lua
local ui = require("metalio_ui")
local delay = require("delay")
local w,h = ui.open()
local running = true
while running do
  local pressed,x,y = ui.touch()
  if pressed and x > w - 120 and y < 80 then running = false end
  -- Update state before drawing.
  ui.begin_frame(0x101820)
  ui.text(w - 110, 20, "QUIT", 0xFFFFFF, 100)
  -- Draw the full current frame.
  ui.end_frame()
  delay.delay_ms(30)
end
ui.close()
```
)skill";

constexpr const char* kMemorySkill = R"skill(---
{
  "name": "memory_ops",
  "description": "Remember, recall, list, update, or forget durable facts using persistent on-device memory.",
  "metadata": {
    "cap_groups": ["claw_memory"],
    "manage_mode": "readonly"
  }
}
---

# Long-term Memory

Use this skill when the user explicitly asks to remember or forget something,
asks what is remembered, or asks for an answer based on persistent memory.

- For an explicit save request, call `memory_store` with a concise normalized
  fact, not a raw quote.
- For recall, inspect the injected summary-label catalog and use exact labels
  with `memory_recall`. Use `memory_list` when the user wants the full list.
- Before updating or forgetting, find the exact `memory_id`, then call
  `memory_update` or `memory_forget`.
- Do not claim a write, update, or deletion succeeded unless its tool succeeded.
- Do not store casual statements unless the user clearly asks for persistence.
- Keep the conversational answer primary and memory-operation reporting brief.
)skill";

constexpr const char* kProfileSkill = R"skill(---
{
  "name": "profile_memory_ops",
  "description": "Persistently edit the assistant persona, identity, interaction style, or standing user profile.",
  "metadata": {
    "cap_groups": ["cap_files"],
    "manage_mode": "readonly"
  }
}
---

# Editable Profile Memory

Use this only when the user explicitly requests a durable change to the
assistant persona/style or to standing assumptions about the user.

- Agent style and role: `/sdcard/esp_claw/memory/soul.md`
- Agent identity: `/sdcard/esp_claw/memory/identity.md`
- Standing user profile: `/sdcard/esp_claw/memory/user.md`

Read the target first, then use `edit_file`. Keep changes concise, structured,
and non-redundant. Ordinary remembered facts belong in the `memory_ops` skill.
)skill";

constexpr const char* kSkillCreatorSkill = R"skill(---
{
  "name": "skill_creator",
  "description": "Create or update a reusable Beaver skill when the user asks to teach the device a repeatable workflow.",
  "metadata": {
    "cap_groups": ["cap_files", "cap_skill"],
    "manage_mode": "readonly"
  }
}
---

# Skill Creator

Create a skill only when the user explicitly asks for a reusable workflow or
when a repeated task clearly needs reusable instructions.

1. Choose a lowercase snake_case `skill_id`.
2. Write `/sdcard/esp_claw/skills/<skill_id>/SKILL.md` with JSON frontmatter
   containing `name`, `description`, and metadata containing `cap_groups` and
   `manage_mode: runtime`.
3. Reference only capability groups already present on this firmware:
   `cap_files`, `cap_lua`, and `claw_memory`.
4. Put Lua helpers under the same skill directory and use
   `{CUR_SKILL_DIR}` in the document to reference them.
5. Call `register_skill` with `skill_id` and
   `file="<skill_id>/SKILL.md"`, then report the result.

Do not claim a new skill can access hardware or network capabilities that are
not installed in this firmware.
)skill";

struct BuiltinSkill {
    const char* id;
    const char* document;
};

constexpr BuiltinSkill kSkills[] = {
    {"metalio_chat_coding", kChatCodingSkill},
    {"memory_ops", kMemorySkill},
    {"profile_memory_ops", kProfileSkill},
    {"skill_creator", kSkillCreatorSkill},
};

esp_err_t EnsureDirectory(const char* path) {
    struct stat info = {};
    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t WriteDocument(const char* root, const BuiltinSkill& skill) {
    char directory[192];
    char path[224];
    char temporary_path[232];
    if (std::snprintf(directory, sizeof(directory), "%s/%s", root, skill.id) >=
            static_cast<int>(sizeof(directory)) ||
        std::snprintf(path, sizeof(path), "%s/SKILL.md", directory) >=
            static_cast<int>(sizeof(path)) ||
        std::snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path) >=
            static_cast<int>(sizeof(temporary_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = EnsureDirectory(directory);
    if (err != ESP_OK) {
        return err;
    }
    FILE* file = std::fopen(temporary_path, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    const size_t length = std::strlen(skill.document);
    const bool ok = std::fwrite(skill.document, 1, length, file) == length &&
                    std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed) {
        std::remove(temporary_path);
        return ESP_FAIL;
    }
    // FatFS does not consistently replace an existing destination on rename.
    // The complete temporary file is ready first, so a power loss can at worst
    // leave this one builtin absent until the next boot reinstalls it.
    if (std::remove(path) != 0 && errno != ENOENT) {
        std::remove(temporary_path);
        return ESP_FAIL;
    }
    if (std::rename(temporary_path, path) != 0) {
        std::remove(temporary_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t InstallEspClawBuiltinSkills(const char* builtin_root) {
    if (builtin_root == nullptr || builtin_root[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = EnsureDirectory(builtin_root);
    if (err != ESP_OK) {
        return err;
    }
    for (const auto& skill : kSkills) {
        err = WriteDocument(builtin_root, skill);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
