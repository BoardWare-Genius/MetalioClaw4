#include "esp_claw_service.h"

#include "application.h"
#include "board.h"
#include "esp_claw_builtin_skills.h"
#include "esp_claw_game_ui.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if CONFIG_ESP_CLAW_AGENT
#include "cap_files.h"
#include "cap_lua.h"
#include "cap_skill_mgr.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_memory.h"
#include "claw_paths.h"
#include "claw_skill.h"
#include "lua_module_delay.h"
#include "lua_module_json.h"
#endif

namespace {

constexpr const char* TAG = "EspClawService";
constexpr const char* kSettingsNamespace = "esp_claw";
constexpr const char* kDefaultSession = "metalio-claw";
constexpr const char* kSdConfigPath = "/sdcard/system/esp_claw.json";
constexpr const char* kChatCodingRoot = "/sdcard/esp_claw";
constexpr const char* kSessionRoot = "/sdcard/esp_claw/sessions";
constexpr const char* kMemoryRoot = "/sdcard/esp_claw/memory";
constexpr const char* kBuiltinSkillsRoot = "/sdcard/esp_claw/.builtin_skills";
constexpr const char* kUserSkillsRoot = "/sdcard/esp_claw/skills";
constexpr const char* kScriptsRoot = "/sdcard/esp_claw/scripts";
constexpr size_t kMaxInjectedSessionHistoryBytes = 20 * 1024;
constexpr long kMaxSdConfigBytes = 4096;

#if CONFIG_ESP_CLAW_AGENT
constexpr const char* kDeviceToolsJson = R"json([
  {
    "type":"function",
    "function":{
      "name":"get_device_status",
      "description":"Read the current Beaverbeaver device, battery, network, agent-mode and background-recording status. This does not change device state.",
      "parameters":{"type":"object","properties":{},"additionalProperties":false}
    }
  },
  {
    "type":"function",
    "function":{
      "name":"get_recording_storage",
      "description":"Count queued background WAV and JPEG files on the SD card and report their total bytes. Uploaded files may already have been removed.",
      "parameters":{"type":"object","properties":{},"additionalProperties":false}
    }
  },
  {
    "type":"function",
    "function":{
      "name":"set_background_recording",
      "description":"Start or stop background recording and optionally select camera, microphone, or both. Call this only when the user explicitly asks to change recording state. Omitted source fields keep the current selection.",
      "parameters":{
        "type":"object",
        "properties":{
          "enabled":{"type":"boolean","description":"true to start recording; false to stop recording"},
          "audio_enabled":{"type":"boolean","description":"record microphone audio"},
          "camera_enabled":{"type":"boolean","description":"record camera snapshots"}
        },
        "required":["enabled"],
        "additionalProperties":false
      }
    }
  },
  {
    "type":"function",
    "function":{
      "name":"list_media_files",
      "description":"List deletable photos and recordings on the SD card. Categories: camera_photos are JPG files shown in the camera gallery; saved_recordings are manually saved OPUS/WAV files; personal_media combines those two user-visible groups; background_photos and background_audio are the protected continuous-recording queues under /sdcard/bgphoto and /sdcard/bgaudio; background_media combines both protected queues. Use another aggregate category to inspect more than one group.",
      "parameters":{
        "type":"object",
        "properties":{
          "category":{"type":"string","enum":["camera_photos","saved_recordings","personal_media","background_photos","background_audio","background_media","all_photos","all_recordings","all_media"]},
          "limit":{"type":"integer","minimum":1,"maximum":20,"description":"Maximum number of filenames to return; defaults to 20."}
        },
        "required":["category"],
        "additionalProperties":false
      }
    }
  },
  {
    "type":"function",
    "function":{
      "name":"delete_media_files",
      "description":"Permanently delete media from the SD card. Call only after the user explicitly asks to delete it. First use list_media_files when the request is ambiguous. Use category background_media when the user asks to delete the protected continuous-recording pictures and audio in /sdcard/bgphoto and /sdcard/bgaudio. selection must be one exact listed basename, or * only when the user explicitly asks to delete every file in the selected category. This tool cannot delete directories, configuration, memory, skills, or arbitrary paths. Background queues can be deleted only after continuous recording has fully stopped.",
      "parameters":{
        "type":"object",
        "properties":{
          "category":{"type":"string","enum":["camera_photos","saved_recordings","personal_media","background_photos","background_audio","background_media","all_photos","all_recordings","all_media"]},
          "selection":{"type":"string","description":"An exact basename returned by list_media_files, or * to delete all matching files."}
        },
        "required":["category","selection"],
        "additionalProperties":false
      }
    }
  },
  {
    "type":"function",
    "function":{
      "name":"reset_conversation",
      "description":"Clear the persistent Beaver conversation history and active skills. Call only when the user explicitly asks to start fresh or clear this conversation. This does not delete long-term memories.",
      "parameters":{"type":"object","properties":{},"additionalProperties":false}
    }
  }
])json";

constexpr const char* kSkillUseInstructions = R"prompt(
Use the Available skills catalog as progressive instructions. When a request
matches a skill, call activate_skill before acting, follow the returned skill
document, and then use the capability tools it enables. Do not claim a device,
file, memory, or program operation succeeded unless its tool call succeeded.
Never delete media unless the user explicitly requested deletion. If it is not
clear which files they mean, list the matching media and ask before deleting.
The user's "protected/system recording pictures and audio" means the
background_media category (/sdcard/bgphoto and /sdcard/bgaudio), not files
under /sdcard/system. Stop continuous recording before deleting that category.
)prompt";

bool s_chat_coding_ready = false;

struct DirectoryStats {
    uint32_t files = 0;
    uint64_t bytes = 0;
};

DirectoryStats GetDirectoryStats(const char* directory, const char* suffix) {
    DirectoryStats result;
    DIR* dir = opendir(directory);
    if (dir == nullptr) {
        return result;
    }
    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        const size_t name_len = std::strlen(name);
        const size_t suffix_len = std::strlen(suffix);
        if (name_len <= suffix_len ||
            std::strcmp(name + name_len - suffix_len, suffix) != 0) {
            continue;
        }
        std::string path = std::string(directory) + "/" + name;
        struct stat info = {};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            ++result.files;
            result.bytes += static_cast<uint64_t>(info.st_size);
        }
    }
    closedir(dir);
    return result;
}

esp_err_t SetJsonOutput(cJSON* root, char** out_output);

enum class MediaCategory {
    kInvalid,
    kCameraPhotos,
    kSavedRecordings,
    kPersonalMedia,
    kBackgroundPhotos,
    kBackgroundAudio,
    kBackgroundMedia,
    kAllPhotos,
    kAllRecordings,
    kAllMedia,
};

struct MediaLocation {
    const char* category;
    const char* directory;
    bool photos;
    bool saved_recordings;
    bool background;
};

constexpr MediaLocation kMediaLocations[] = {
    {"camera_photos", "/sdcard", true, false, false},
    {"saved_recordings", "/sdcard/recordings", false, true, false},
    {"background_photos", "/sdcard/bgphoto", true, false, true},
    {"background_audio", "/sdcard/bgaudio", false, false, true},
};

MediaCategory ParseMediaCategory(const char* category) {
    if (category == nullptr) {
        return MediaCategory::kInvalid;
    }
    if (std::strcmp(category, "camera_photos") == 0) {
        return MediaCategory::kCameraPhotos;
    }
    if (std::strcmp(category, "saved_recordings") == 0) {
        return MediaCategory::kSavedRecordings;
    }
    if (std::strcmp(category, "personal_media") == 0) {
        return MediaCategory::kPersonalMedia;
    }
    if (std::strcmp(category, "background_photos") == 0) {
        return MediaCategory::kBackgroundPhotos;
    }
    if (std::strcmp(category, "background_audio") == 0) {
        return MediaCategory::kBackgroundAudio;
    }
    if (std::strcmp(category, "background_media") == 0) {
        return MediaCategory::kBackgroundMedia;
    }
    if (std::strcmp(category, "all_photos") == 0) {
        return MediaCategory::kAllPhotos;
    }
    if (std::strcmp(category, "all_recordings") == 0) {
        return MediaCategory::kAllRecordings;
    }
    if (std::strcmp(category, "all_media") == 0) {
        return MediaCategory::kAllMedia;
    }
    return MediaCategory::kInvalid;
}

bool CategoryIncludes(const MediaLocation& location, MediaCategory category) {
    switch (category) {
        case MediaCategory::kCameraPhotos:
            return std::strcmp(location.category, "camera_photos") == 0;
        case MediaCategory::kSavedRecordings:
            return std::strcmp(location.category, "saved_recordings") == 0;
        case MediaCategory::kPersonalMedia:
            return !location.background;
        case MediaCategory::kBackgroundPhotos:
            return std::strcmp(location.category, "background_photos") == 0;
        case MediaCategory::kBackgroundAudio:
            return std::strcmp(location.category, "background_audio") == 0;
        case MediaCategory::kBackgroundMedia:
            return location.background;
        case MediaCategory::kAllPhotos:
            return location.photos;
        case MediaCategory::kAllRecordings:
            return !location.photos;
        case MediaCategory::kAllMedia:
            return true;
        default:
            return false;
    }
}

bool CategoryIncludesBackground(MediaCategory category) {
    for (const auto& location : kMediaLocations) {
        if (location.background && CategoryIncludes(location, category)) {
            return true;
        }
    }
    return false;
}

bool HasCaseInsensitiveSuffix(const char* name, const char* suffix) {
    if (name == nullptr || suffix == nullptr) {
        return false;
    }
    const size_t name_length = std::strlen(name);
    const size_t suffix_length = std::strlen(suffix);
    return name_length > suffix_length &&
           strcasecmp(name + name_length - suffix_length, suffix) == 0;
}

bool IsMediaFilename(const MediaLocation& location, const char* name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    if (location.photos) {
        return HasCaseInsensitiveSuffix(name, ".jpg") ||
               (location.background &&
                HasCaseInsensitiveSuffix(name, ".jpg.rejected"));
    }
    return HasCaseInsensitiveSuffix(name, ".wav") ||
           (location.saved_recordings &&
            HasCaseInsensitiveSuffix(name, ".opus")) ||
           (location.background &&
            HasCaseInsensitiveSuffix(name, ".wav.rejected"));
}

bool IsSafeMediaSelection(const char* selection) {
    if (selection == nullptr || selection[0] == '\0') {
        return false;
    }
    if (std::strcmp(selection, "*") == 0) {
        return true;
    }
    return std::strcmp(selection, ".") != 0 &&
           std::strcmp(selection, "..") != 0 &&
           std::strchr(selection, '/') == nullptr &&
           std::strchr(selection, '\\') == nullptr;
}

void RemoveRecordingSidecar(const char* media_path) {
    char sidecar[384];
    const int length = std::snprintf(sidecar, sizeof(sidecar), "%s.dur", media_path);
    if (length > 0 && static_cast<size_t>(length) < sizeof(sidecar)) {
        unlink(sidecar);
    }
}

esp_err_t ListMediaFiles(MediaCategory category, int limit, char** out_output) {
    cJSON* root = cJSON_CreateObject();
    cJSON* files = cJSON_CreateArray();
    if (root == nullptr || files == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(files);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "files", files);
    uint32_t total_files = 0;
    uint64_t total_bytes = 0;
    int returned = 0;

    for (const auto& location : kMediaLocations) {
        if (!CategoryIncludes(location, category)) {
            continue;
        }
        DIR* dir = opendir(location.directory);
        if (dir == nullptr) {
            continue;
        }
        while (dirent* entry = readdir(dir)) {
            if (!IsMediaFilename(location, entry->d_name)) {
                continue;
            }
            std::string path = std::string(location.directory) + "/" + entry->d_name;
            struct stat info = {};
            if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
                continue;
            }
            ++total_files;
            total_bytes += static_cast<uint64_t>(info.st_size);
            if (returned >= limit) {
                continue;
            }
            cJSON* file = cJSON_CreateObject();
            if (file == nullptr) {
                closedir(dir);
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(file, "category", location.category);
            cJSON_AddStringToObject(file, "name", entry->d_name);
            cJSON_AddNumberToObject(file, "bytes", static_cast<double>(info.st_size));
            cJSON_AddItemToArray(files, file);
            ++returned;
        }
        closedir(dir);
    }
    cJSON_AddNumberToObject(root, "total_files", total_files);
    cJSON_AddNumberToObject(root, "total_bytes", static_cast<double>(total_bytes));
    cJSON_AddNumberToObject(root, "returned", returned);
    cJSON_AddBoolToObject(root, "truncated", total_files > static_cast<uint32_t>(returned));
    return SetJsonOutput(root, out_output);
}

esp_err_t DeleteMediaFiles(MediaCategory category, const char* selection,
                           char** out_output) {
    const bool delete_all = std::strcmp(selection, "*") == 0;
    uint32_t matched = 0;
    uint32_t deleted = 0;
    uint32_t failed = 0;
    uint64_t bytes_deleted = 0;

    for (const auto& location : kMediaLocations) {
        if (!CategoryIncludes(location, category)) {
            continue;
        }
        DIR* dir = opendir(location.directory);
        if (dir == nullptr) {
            continue;
        }
        while (dirent* entry = readdir(dir)) {
            if (!IsMediaFilename(location, entry->d_name) ||
                (!delete_all && std::strcmp(entry->d_name, selection) != 0)) {
                continue;
            }
            std::string path = std::string(location.directory) + "/" + entry->d_name;
            struct stat info = {};
            if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
                continue;
            }
            ++matched;
            if (unlink(path.c_str()) == 0) {
                ++deleted;
                bytes_deleted += static_cast<uint64_t>(info.st_size);
                if (location.saved_recordings) {
                    RemoveRecordingSidecar(path.c_str());
                }
            } else {
                ++failed;
                ESP_LOGW(TAG, "Failed to delete media %s: errno=%d %s",
                         path.c_str(), errno, strerror(errno));
            }
        }
        closedir(dir);
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", failed == 0);
    cJSON_AddNumberToObject(result, "matched", matched);
    cJSON_AddNumberToObject(result, "deleted", deleted);
    cJSON_AddNumberToObject(result, "failed", failed);
    cJSON_AddNumberToObject(result, "bytes_deleted",
                            static_cast<double>(bytes_deleted));
    if (matched == 0) {
        cJSON_AddStringToObject(result, "note", "no matching media files");
    }
    return SetJsonOutput(result, out_output);
}

esp_err_t SetJsonOutput(cJSON* root, char** out_output) {
    if (out_output == nullptr) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (root == nullptr) {
        *out_output = nullptr;
        return ESP_ERR_NO_MEM;
    }
    *out_output = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_output != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t CollectDeviceTools(const claw_core_request_t* /*request*/,
                             claw_core_context_t* out_context,
                             void* /*user_ctx*/) {
    if (out_context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_context = {};
    out_context->kind = CLAW_CORE_CONTEXT_KIND_TOOLS;
    out_context->content = strdup(kDeviceToolsJson);
    return out_context->content != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t CollectSkillUseInstructions(const claw_core_request_t* /*request*/,
                                      claw_core_context_t* out_context,
                                      void* /*user_ctx*/) {
    if (out_context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_context = {};
    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = strdup(kSkillUseInstructions);
    return out_context->content != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

bool RangeContains(const char* begin, const char* end, const char* needle) {
    const size_t needle_length = std::strlen(needle);
    if (needle_length == 0 || end <= begin ||
        static_cast<size_t>(end - begin) < needle_length) {
        return false;
    }
    for (const char* cursor = begin; cursor + needle_length <= end; ++cursor) {
        if (std::memcmp(cursor, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t CollectBoundedSessionHistory(const claw_core_request_t* request,
                                       claw_core_context_t* out_context,
                                       void* user_ctx) {
    esp_err_t error = claw_memory_session_history_provider.collect(
        request, out_context, user_ctx);
    if (error != ESP_OK || out_context == nullptr ||
        out_context->content == nullptr) {
        return error;
    }

    char* history = out_context->content;
    const size_t length = std::strlen(history);
    if (length <= kMaxInjectedSessionHistoryBytes || length < 3 ||
        history[0] != '[' || history[length - 1] != ']') {
        return ESP_OK;
    }

    const size_t target = length - kMaxInjectedSessionHistoryBytes;
    const char* fallback_start = nullptr;
    const char* user_start = nullptr;
    const char* element_start = history + 1;
    bool in_string = false;
    bool escaped = false;
    int depth = 1;

    for (char* cursor = history + 1; cursor < history + length - 1;
         ++cursor) {
        const char ch = *cursor;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '[' || ch == '{') {
            ++depth;
        } else if (ch == ']' || ch == '}') {
            --depth;
        } else if (ch == ',' && depth == 1) {
            if (static_cast<size_t>(element_start - history) >= target) {
                if (fallback_start == nullptr) {
                    fallback_start = element_start;
                }
                if (RangeContains(element_start, cursor,
                                  "\"role\":\"user\"") ||
                    RangeContains(element_start, cursor,
                                  "\"role\": \"user\"")) {
                    user_start = element_start;
                    break;
                }
            }
            const char* next_start = cursor + 1;
            while (next_start < history + length - 1 &&
                   (*next_start == ' ' || *next_start == '\n' ||
                    *next_start == '\r' || *next_start == '\t')) {
                ++next_start;
            }
            element_start = next_start;
        }
    }

    if (user_start == nullptr &&
        static_cast<size_t>(element_start - history) >= target) {
        if (fallback_start == nullptr) {
            fallback_start = element_start;
        }
        if (RangeContains(element_start, history + length - 1,
                          "\"role\":\"user\"") ||
            RangeContains(element_start, history + length - 1,
                          "\"role\": \"user\"")) {
            user_start = element_start;
        }
    }

    const char* selected = user_start != nullptr ? user_start : fallback_start;
    if (selected == nullptr) {
        return ESP_OK;
    }
    const size_t retained = static_cast<size_t>(history + length - selected);
    std::memmove(history + 1, selected, retained + 1);
    history[0] = '[';
    ESP_LOGW(TAG, "Trimmed session history context from %u to %u bytes",
             static_cast<unsigned>(length),
             static_cast<unsigned>(std::strlen(history)));
    return ESP_OK;
}

bool IsDeviceTool(const char* name) {
    return name != nullptr &&
           (std::strcmp(name, "get_device_status") == 0 ||
            std::strcmp(name, "get_recording_storage") == 0 ||
            std::strcmp(name, "set_background_recording") == 0 ||
            std::strcmp(name, "list_media_files") == 0 ||
            std::strcmp(name, "delete_media_files") == 0 ||
            std::strcmp(name, "reset_conversation") == 0);
}

esp_err_t CallDeviceTool(const char* cap_name,
                         const char* input_json,
                         const claw_core_request_t* /*request*/,
                         char** out_output,
                         void* user_ctx) {
    if (cap_name == nullptr || out_output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_output = nullptr;
    auto& app = Application::GetInstance();

    if (std::strcmp(cap_name, "get_device_status") == 0) {
        cJSON* root = cJSON_Parse(Board::GetInstance().GetDeviceStatusJson().c_str());
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            root = cJSON_CreateObject();
        }
        cJSON_AddBoolToObject(root, "background_recording",
                              app.IsBackgroundRecording());
        cJSON_AddBoolToObject(root, "background_audio_enabled",
                              app.IsBackgroundAudioEnabled());
        cJSON_AddBoolToObject(root, "background_camera_enabled",
                              app.IsBackgroundCameraEnabled());
        cJSON_AddStringToObject(root, "agent_mode",
                                app.IsEspClawMode() ? "esp_claw"
                                                   : "digital_person");
        cJSON_AddBoolToObject(root, "esp_claw_configured",
                              app.IsEspClawConfigured());
        return SetJsonOutput(root, out_output);
    }

    if (std::strcmp(cap_name, "get_recording_storage") == 0) {
        const DirectoryStats audio = GetDirectoryStats("/sdcard/bgaudio", ".wav");
        const DirectoryStats photos = GetDirectoryStats("/sdcard/bgphoto", ".jpg");
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "audio_files", audio.files);
        cJSON_AddNumberToObject(root, "audio_bytes",
                                static_cast<double>(audio.bytes));
        cJSON_AddNumberToObject(root, "photo_files", photos.files);
        cJSON_AddNumberToObject(root, "photo_bytes",
                                static_cast<double>(photos.bytes));
        cJSON_AddNumberToObject(root, "total_bytes",
                                static_cast<double>(audio.bytes + photos.bytes));
        return SetJsonOutput(root, out_output);
    }

    if (std::strcmp(cap_name, "list_media_files") == 0) {
        cJSON* args = cJSON_Parse(input_json != nullptr ? input_json : "{}");
        const cJSON* category_item =
            cJSON_GetObjectItemCaseSensitive(args, "category");
        const cJSON* limit_item = cJSON_GetObjectItemCaseSensitive(args, "limit");
        const MediaCategory category = ParseMediaCategory(
            cJSON_IsString(category_item) ? category_item->valuestring : nullptr);
        int limit = cJSON_IsNumber(limit_item) ? limit_item->valueint : 20;
        cJSON_Delete(args);
        if (category == MediaCategory::kInvalid || limit < 1 || limit > 20) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "ok", false);
            cJSON_AddStringToObject(error, "error",
                                    "invalid media category or limit");
            SetJsonOutput(error, out_output);
            return ESP_ERR_INVALID_ARG;
        }
        return ListMediaFiles(category, limit, out_output);
    }

    if (std::strcmp(cap_name, "delete_media_files") == 0) {
        cJSON* args = cJSON_Parse(input_json != nullptr ? input_json : "{}");
        const cJSON* category_item =
            cJSON_GetObjectItemCaseSensitive(args, "category");
        const cJSON* selection_item =
            cJSON_GetObjectItemCaseSensitive(args, "selection");
        const MediaCategory category = ParseMediaCategory(
            cJSON_IsString(category_item) ? category_item->valuestring : nullptr);
        const char* selection_value =
            cJSON_IsString(selection_item) ? selection_item->valuestring : nullptr;
        char selection[256] = {};
        const bool selection_fits =
            selection_value != nullptr &&
            std::strlen(selection_value) < sizeof(selection);
        if (selection_fits) {
            strlcpy(selection, selection_value, sizeof(selection));
        }
        cJSON_Delete(args);
        if (category == MediaCategory::kInvalid ||
            !selection_fits ||
            !IsSafeMediaSelection(selection)) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "ok", false);
            cJSON_AddStringToObject(error, "error",
                                    "invalid media category or selection");
            SetJsonOutput(error, out_output);
            return ESP_ERR_INVALID_ARG;
        }
        if (CategoryIncludesBackground(category) &&
            (app.IsBackgroundRecording() ||
             app.IsBackgroundRecordingTogglePending())) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "ok", false);
            cJSON_AddStringToObject(
                error, "error",
                "continuous recording must be fully stopped before deleting background media");
            cJSON_AddBoolToObject(error, "background_recording",
                                  app.IsBackgroundRecording());
            cJSON_AddBoolToObject(error, "stop_pending",
                                  app.IsBackgroundRecordingTogglePending());
            SetJsonOutput(error, out_output);
            return ESP_ERR_INVALID_STATE;
        }
        return DeleteMediaFiles(category, selection, out_output);
    }

    if (std::strcmp(cap_name, "set_background_recording") == 0) {
        cJSON* args = cJSON_Parse(input_json != nullptr ? input_json : "{}");
        cJSON* enabled = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        cJSON* audio_enabled =
            cJSON_GetObjectItemCaseSensitive(args, "audio_enabled");
        cJSON* camera_enabled =
            cJSON_GetObjectItemCaseSensitive(args, "camera_enabled");
        if (!cJSON_IsBool(enabled)) {
            cJSON_Delete(args);
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "ok", false);
            cJSON_AddStringToObject(error, "error",
                                    "enabled must be a boolean");
            SetJsonOutput(error, out_output);
            return ESP_ERR_INVALID_ARG;
        }
        if ((audio_enabled != nullptr && !cJSON_IsBool(audio_enabled)) ||
            (camera_enabled != nullptr && !cJSON_IsBool(camera_enabled))) {
            cJSON_Delete(args);
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "ok", false);
            cJSON_AddStringToObject(
                error, "error",
                "audio_enabled and camera_enabled must be booleans");
            SetJsonOutput(error, out_output);
            return ESP_ERR_INVALID_ARG;
        }
        const bool requested = cJSON_IsTrue(enabled);
        const bool requested_audio = audio_enabled != nullptr
                                         ? cJSON_IsTrue(audio_enabled)
                                         : app.IsBackgroundAudioEnabled();
        const bool requested_camera = camera_enabled != nullptr
                                          ? cJSON_IsTrue(camera_enabled)
                                          : app.IsBackgroundCameraEnabled();
        cJSON_Delete(args);

        if (!requested_audio && !requested_camera) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "ok", false);
            cJSON_AddStringToObject(error, "error",
                                    "at least one recording source is required");
            SetJsonOutput(error, out_output);
            return ESP_ERR_INVALID_ARG;
        }

        const bool needs_update =
            app.IsBackgroundRecording() != requested ||
            app.IsBackgroundAudioEnabled() != requested_audio ||
            app.IsBackgroundCameraEnabled() != requested_camera;
        bool accepted = true;
        if (needs_update) {
            accepted = app.SetBackgroundRecording(
                requested, requested_audio, requested_camera);
            for (int i = 0; accepted && i < 100 &&
                            app.IsBackgroundRecordingTogglePending(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        const bool actual = app.IsBackgroundRecording();
        const bool actual_audio = app.IsBackgroundAudioEnabled();
        const bool actual_camera = app.IsBackgroundCameraEnabled();
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(
            result, "ok",
            accepted && actual == requested && actual_audio == requested_audio &&
                actual_camera == requested_camera);
        cJSON_AddBoolToObject(result, "requested", requested);
        cJSON_AddBoolToObject(result, "background_recording", actual);
        cJSON_AddBoolToObject(result, "audio_enabled", actual_audio);
        cJSON_AddBoolToObject(result, "camera_enabled", actual_camera);
        return SetJsonOutput(result, out_output);
    }

    if (std::strcmp(cap_name, "reset_conversation") == 0) {
        auto* service = static_cast<EspClawService*>(user_ctx);
        cJSON* result = cJSON_CreateObject();
        const bool ok = service != nullptr && s_chat_coding_ready;
        if (ok) {
            service->ScheduleConversationReset();
        }
        cJSON_AddBoolToObject(result, "ok", ok);
        cJSON_AddBoolToObject(result, "reset_after_response", ok);
        cJSON_AddBoolToObject(result, "long_term_memory_preserved", true);
        if (!ok) {
            cJSON_AddStringToObject(
                result, "error",
                "persistent conversation storage is unavailable");
        }
        const esp_err_t output_error = SetJsonOutput(result, out_output);
        return output_error != ESP_OK
                   ? output_error
                   : (ok ? ESP_OK : ESP_FAIL);
    }

    cJSON* error = cJSON_CreateObject();
    cJSON_AddBoolToObject(error, "ok", false);
    cJSON_AddStringToObject(error, "error", "unsupported device tool");
    SetJsonOutput(error, out_output);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t SetTextOutput(const char* text, char** out_output) {
    if (out_output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_output = strdup(text != nullptr ? text : "");
    return *out_output != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t GetLuaJobStatusSafely(const char* input_json, char** out_output) {
    if (out_output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_output = nullptr;
    cJSON* root = cJSON_Parse(input_json != nullptr ? input_json : "{}");
    const cJSON* job_id_item =
        cJSON_GetObjectItemCaseSensitive(root, "job_id");
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    const char* requested = cJSON_IsString(job_id_item)
                                ? job_id_item->valuestring
                                : (cJSON_IsString(name_item)
                                       ? name_item->valuestring
                                       : nullptr);
    if (requested == nullptr || requested[0] == '\0') {
        cJSON_Delete(root);
        SetTextOutput("Error: provide either 'job_id' or 'name'", out_output);
        return ESP_ERR_INVALID_ARG;
    }

    constexpr size_t kListingBytes = 4096;
    char* listing = static_cast<char*>(heap_caps_calloc_prefer(
        1, kListingBytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (listing == nullptr) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = cap_lua_list_jobs("all", listing, kListingBytes);
    if (error != ESP_OK) {
        std::free(listing);
        cJSON_Delete(root);
        return error;
    }

    const char* match = std::strstr(listing, requested);
    if (match == nullptr) {
        std::free(listing);
        cJSON_Delete(root);
        SetTextOutput("Error: Lua async job not found", out_output);
        return ESP_ERR_NOT_FOUND;
    }
    const char* line_start = match;
    while (line_start > listing && line_start[-1] != '\n') {
        --line_start;
    }
    const char* line_end = std::strchr(match, '\n');
    if (line_end == nullptr) {
        line_end = listing + std::strlen(listing);
    }
    const size_t line_length = static_cast<size_t>(line_end - line_start);
    char* result = static_cast<char*>(std::malloc(line_length + 128));
    if (result == nullptr) {
        std::free(listing);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    std::snprintf(result, line_length + 128,
                  "%.*s\nrecent_log=disabled_on_low_memory_device",
                  static_cast<int>(line_length), line_start);
    std::free(listing);
    cJSON_Delete(root);
    *out_output = result;
    return ESP_OK;
}

bool IsChatCodingLuaPathAllowed(const char* input_json) {
    cJSON* root = cJSON_Parse(input_json != nullptr ? input_json : "{}");
    const cJSON* path = cJSON_GetObjectItemCaseSensitive(root, "path");
    const char* value = cJSON_IsString(path) ? path->valuestring : nullptr;
    const size_t root_length = std::strlen(kChatCodingRoot);
    const bool allowed = value != nullptr &&
                         std::strncmp(value, kChatCodingRoot, root_length) == 0 &&
                         value[root_length] == '/' &&
                         std::strstr(value, "..") == nullptr;
    cJSON_Delete(root);
    return allowed;
}

esp_err_t CallEspClawTool(const char* cap_name,
                          const char* input_json,
                          const claw_core_request_t* request,
                          char** out_output,
                          void* user_ctx) {
    if (cap_name == nullptr || out_output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* service = static_cast<EspClawService*>(user_ctx);
    if (service != nullptr) {
        service->ReportToolStarted(cap_name);
    }
    esp_err_t result = ESP_OK;
    if (IsDeviceTool(cap_name)) {
        result = CallDeviceTool(cap_name, input_json, request, out_output,
                                user_ctx);
    } else if (!s_chat_coding_ready) {
        SetTextOutput("Chat Coding tools are unavailable; check the SD card mount",
                      out_output);
        result = ESP_ERR_INVALID_STATE;
    } else if ((std::strcmp(cap_name, "lua_run_script") == 0 ||
                std::strcmp(cap_name, "lua_run_script_async") == 0) &&
               !IsChatCodingLuaPathAllowed(input_json)) {
        SetTextOutput("Lua scripts must be under /sdcard/esp_claw/ and may not contain '..'",
                      out_output);
        result = ESP_ERR_INVALID_ARG;
    } else if (std::strcmp(cap_name, "lua_get_async_job") == 0 ||
               std::strcmp(cap_name, "lua_tail_async_job") == 0) {
        // The upstream detailed formatter can exhaust/corrupt the constrained
        // internal stack while a large agent context is resident. Return the
        // compact registry status instead; source inspection remains available
        // through read_file when diagnosis is needed.
        result = GetLuaJobStatusSafely(input_json, out_output);
    } else {
        result = claw_cap_call_from_core(cap_name, input_json, request,
                                         out_output, nullptr);
    }
    if (service != nullptr) {
        service->ReportToolFinished(cap_name, result);
    }
    return result;
}

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

esp_err_t RestoreSkillVisibility(const char* session_id) {
    char** group_ids = nullptr;
    size_t group_count = 0;
    esp_err_t error = claw_skill_load_active_cap_groups(
        session_id, &group_ids, &group_count);
    if (error == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    error = claw_cap_set_session_llm_visible_groups(
        session_id, const_cast<const char* const*>(group_ids), group_count);
    for (size_t i = 0; i < group_count; ++i) {
        std::free(group_ids[i]);
    }
    std::free(group_ids);
    return error;
}

esp_err_t InitializeChatCoding(const std::string& api_key,
                               const std::string& base_url,
                               const std::string& model,
                               const std::string& backend) {
    if (s_chat_coding_ready) {
        return ESP_OK;
    }
    const size_t initial_internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    struct stat sd_info = {};
    if (stat("/sdcard", &sd_info) != 0 || !S_ISDIR(sd_info.st_mode)) {
        ESP_LOGW(TAG, "Chat Coding disabled: /sdcard is not mounted");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t error = EnsureDirectory(kChatCodingRoot);
    if (error != ESP_OK) {
        return error;
    }
    const char* directories[] = {
        kSessionRoot,
        kMemoryRoot,
        kBuiltinSkillsRoot,
        kUserSkillsRoot,
        kScriptsRoot,
    };
    for (const char* directory : directories) {
        error = EnsureDirectory(directory);
        if (error != ESP_OK) {
            return error;
        }
    }
    error = InstallEspClawBuiltinSkills(kBuiltinSkillsRoot);
    if (error != ESP_OK) {
        return error;
    }

    error = claw_paths_set(CLAW_PATH_DATA, kChatCodingRoot);
    if (error != ESP_OK) {
        return error;
    }
    error = claw_cap_init();
    if (error != ESP_OK) {
        return error;
    }
    claw_memory_config_t memory_config = {};
    memory_config.session_root_dir = kSessionRoot;
    memory_config.memory_root_dir = kMemoryRoot;
    memory_config.max_message_chars = 4096;
    memory_config.max_tool_iterations = 16;
    memory_config.llm.api_key = api_key.c_str();
    memory_config.llm.backend_type = backend.c_str();
    memory_config.llm.model = model.c_str();
    memory_config.llm.base_url = base_url.c_str();
    memory_config.llm.auth_type = "bearer";
    memory_config.llm.max_tokens_field = "max_tokens";
    memory_config.llm.timeout_ms = 60000;
    memory_config.llm.max_tokens = 4096;
    memory_config.llm.supports_tools = true;
    memory_config.enable_async_extract_stage_note = false;
    error = claw_memory_init(&memory_config);
    if (error != ESP_OK) {
        return error;
    }
    claw_skill_config_t skill_config = {};
    skill_config.session_state_root_dir = kSessionRoot;
    skill_config.max_file_bytes = 20 * 1024;
    error = claw_skill_init(&skill_config);
    if (error != ESP_OK) {
        return error;
    }
    error = claw_skill_add_directory(kBuiltinSkillsRoot);
    if (error != ESP_OK) {
        return error;
    }
    error = claw_skill_add_directory(kUserSkillsRoot);
    if (error != ESP_OK) {
        return error;
    }
    error = lua_module_delay_register();
    if (error != ESP_OK) {
        return error;
    }
    error = lua_module_json_register();
    if (error != ESP_OK) {
        return error;
    }
    error = RegisterEspClawGameUiModule();
    if (error != ESP_OK) {
        return error;
    }
    error = cap_files_register_group();
    if (error != ESP_OK) {
        return error;
    }
    error = cap_lua_register_group();
    if (error != ESP_OK) {
        return error;
    }
    error = claw_memory_register_group();
    if (error != ESP_OK) {
        return error;
    }
    error = cap_skill_mgr_register_group(kUserSkillsRoot);
    if (error != ESP_OK) {
        return error;
    }
    // Keep only skill discovery globally visible. Activating a skill adds its
    // capability groups for this stable device session.
    const char* visible_groups[] = {"cap_skill"};
    error = claw_cap_set_llm_visible_groups(
        visible_groups, sizeof(visible_groups) / sizeof(visible_groups[0]));
    if (error != ESP_OK) {
        return error;
    }
    error = claw_cap_start_all();
    if (error != ESP_OK) {
        return error;
    }
    error = RestoreSkillVisibility(kDefaultSession);
    if (error != ESP_OK) {
        return error;
    }

    claw_cap_state_t files_state = CLAW_CAP_STATE_REGISTERED;
    claw_cap_state_t lua_state = CLAW_CAP_STATE_REGISTERED;
    claw_cap_state_t memory_state = CLAW_CAP_STATE_REGISTERED;
    claw_cap_state_t skill_state = CLAW_CAP_STATE_REGISTERED;
    if (claw_cap_get_group_state("cap_files", &files_state) != ESP_OK ||
        claw_cap_get_group_state("cap_lua", &lua_state) != ESP_OK ||
        claw_cap_get_group_state("claw_memory", &memory_state) != ESP_OK ||
        claw_cap_get_group_state("cap_skill", &skill_state) != ESP_OK ||
        files_state != CLAW_CAP_STATE_STARTED ||
        lua_state != CLAW_CAP_STATE_STARTED ||
        memory_state != CLAW_CAP_STATE_STARTED ||
        skill_state != CLAW_CAP_STATE_STARTED) {
        ESP_LOGE(TAG,
                 "Agent groups failed: files=%d lua=%d memory=%d skill=%d",
                 static_cast<int>(files_state), static_cast<int>(lua_state),
                 static_cast<int>(memory_state), static_cast<int>(skill_state));
        return ESP_ERR_INVALID_STATE;
    }

    s_chat_coding_ready = true;
    ESP_LOGI(TAG,
             "Agent memory, skills and Chat Coding ready at %s; internal heap %u -> %u bytes",
             kChatCodingRoot, static_cast<unsigned>(initial_internal_free),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    return ESP_OK;
}
#endif

void OverrideJsonString(const cJSON* root, const char* key,
                        std::string& value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
        value = item->valuestring;
    }
}

void LoadSdConfig(std::string& api_key, std::string& base_url,
                  std::string& model, std::string& backend) {
    FILE* file = std::fopen(kSdConfigPath, "rb");
    if (file == nullptr) {
        return;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return;
    }
    const long size = std::ftell(file);
    if (size <= 0 || size > kMaxSdConfigBytes ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Ignoring invalid %s size=%ld", kSdConfigPath, size);
        std::fclose(file);
        return;
    }
    std::vector<char> data(static_cast<size_t>(size) + 1, '\0');
    const size_t read = std::fread(data.data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(size)) {
        ESP_LOGW(TAG, "Failed to read complete %s", kSdConfigPath);
        return;
    }

    cJSON* root = cJSON_ParseWithLength(data.data(), read);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Ignoring malformed %s", kSdConfigPath);
        cJSON_Delete(root);
        return;
    }
    OverrideJsonString(root, "api_key", api_key);
    OverrideJsonString(root, "base_url", base_url);
    OverrideJsonString(root, "model", model);
    OverrideJsonString(root, "backend", backend);
    cJSON_Delete(root);
}

}  // namespace

EspClawService::~EspClawService() {
    Stop();
}

void EspClawService::Initialize(ResponseCallback on_response,
                                ErrorCallback on_error,
                                ProgressCallback on_progress) {
    on_response_ = std::move(on_response);
    on_error_ = std::move(on_error);
    on_progress_ = std::move(on_progress);
    LoadConfig();
}

void EspClawService::LoadConfig() {
#if CONFIG_ESP_CLAW_AGENT
    api_key_ = CONFIG_ESP_CLAW_LLM_API_KEY;
    base_url_ = CONFIG_ESP_CLAW_LLM_BASE_URL;
    model_ = CONFIG_ESP_CLAW_LLM_MODEL;
    backend_ = CONFIG_ESP_CLAW_LLM_BACKEND;
    LoadSdConfig(api_key_, base_url_, model_, backend_);

    // Locally provisioned NVS values take precedence over the SD file, which
    // in turn takes precedence over build-time defaults.
    Settings settings(kSettingsNamespace, false);
    api_key_ = settings.GetString("api_key", api_key_);
    base_url_ = settings.GetString("base_url", base_url_);
    model_ = settings.GetString("model", model_);
    backend_ = settings.GetString("backend", backend_);
    configured_.store(!api_key_.empty() && !base_url_.empty() &&
                      !model_.empty() && !backend_.empty());
#else
    configured_.store(false);
#endif
}

bool EspClawService::Configure(const std::string& api_key,
                               const std::string& base_url,
                               const std::string& model,
                               const std::string& backend) {
#if !CONFIG_ESP_CLAW_AGENT
    (void)api_key;
    (void)base_url;
    (void)model;
    (void)backend;
    return false;
#else
    if (api_key.empty() || base_url.empty() || model.empty() || backend.empty()) {
        NotifyError("Beaver 配置不完整");
        return false;
    }

    const bool restart = IsRunning();
    if (restart) {
        Stop();
    }
    {
        Settings settings(kSettingsNamespace, true);
        settings.SetString("api_key", api_key);
        settings.SetString("base_url", base_url);
        settings.SetString("model", model);
        settings.SetString("backend", backend);
    }
    LoadConfig();
    return !restart || Start();
#endif
}

bool EspClawService::Start() {
#if !CONFIG_ESP_CLAW_AGENT
    NotifyError("固件未启用 Beaver");
    return false;
#else
    if (running_.load()) {
        return true;
    }
    if (core_ != nullptr) {
        NotifyError("Beaver 上一次运行仍在退出，请稍后重试");
        return false;
    }
    LoadConfig();
    if (!configured_.load()) {
        NotifyError("请先配置 Beaver 的 API 地址、密钥和模型");
        return false;
    }

    const esp_err_t chat_coding_error = InitializeChatCoding(
        api_key_, base_url_, model_, backend_);
    if (chat_coding_error != ESP_OK) {
        ESP_LOGW(TAG, "Starting without Chat Coding tools: %s",
                 esp_err_to_name(chat_coding_error));
    }

    claw_core_config_t config = {};
    config.instance_id = 1;
    config.api_key = api_key_.c_str();
    config.backend_type = backend_.c_str();
    config.model = model_.c_str();
    config.base_url = base_url_.c_str();
    config.auth_type = "bearer";
    config.max_tokens_field = "max_tokens";
    config.timeout_ms = 60000;
    config.max_tokens = 4096;
    config.supports_tools = true;
    config.supports_vision = false;
    config.system_prompt = CONFIG_ESP_CLAW_SYSTEM_PROMPT;
    config.task_stack_size = 16 * 1024;
    config.task_priority = 5;
    config.task_core = tskNO_AFFINITY;
    config.request_queue_len = 4;
    config.response_queue_len = 4;
    config.max_tool_iterations = 16;
    config.max_context_providers = 8;
    if (s_chat_coding_ready) {
        config.persist_context = claw_memory_persist_context_callback;
        config.request_gate = claw_memory_request_gate_callback;
    }
    config.call_cap = CallEspClawTool;
    config.cap_user_ctx = this;

    claw_core_handle_t core = nullptr;
    esp_err_t err = claw_core_create(&config, &core);
    if (err == ESP_OK) {
        claw_core_context_provider_t tools_provider = {};
        tools_provider.name = "Metalio Device Tools";
        tools_provider.collect = CollectDeviceTools;
        tools_provider.user_ctx = this;
        err = claw_core_add_context_provider(core, &tools_provider);
    }
    if (err == ESP_OK && s_chat_coding_ready) {
        err = claw_core_add_context_provider(
            core, &claw_memory_profile_provider);
    }
    if (err == ESP_OK && s_chat_coding_ready) {
        err = claw_core_add_context_provider(
            core, &claw_memory_long_term_lightweight_provider);
    }
    if (err == ESP_OK && s_chat_coding_ready) {
        claw_core_context_provider_t history_provider = {};
        history_provider.name = "Metalio Bounded Session History";
        history_provider.collect = CollectBoundedSessionHistory;
        history_provider.flags =
            CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY;
        err = claw_core_add_context_provider(core, &history_provider);
    }
    if (err == ESP_OK && s_chat_coding_ready) {
        err = claw_core_add_context_provider(
            core, &claw_skill_skills_list_provider);
    }
    if (err == ESP_OK && s_chat_coding_ready) {
        err = claw_core_add_context_provider(
            core, &claw_cap_root_agent_tools_provider);
    }
    if (err == ESP_OK && s_chat_coding_ready) {
        claw_core_context_provider_t skill_guide_provider = {};
        skill_guide_provider.name = "Metalio Skill Use Guide";
        skill_guide_provider.collect = CollectSkillUseInstructions;
        skill_guide_provider.user_ctx = this;
        err = claw_core_add_context_provider(core, &skill_guide_provider);
    }
    if (err == ESP_OK) {
        err = claw_core_start(core);
    }
    if (err != ESP_OK) {
        if (core != nullptr) {
            claw_core_destroy(core);
        }
        NotifyError(std::string("Beaver 启动失败: ") + esp_err_to_name(err));
        return false;
    }

    core_ = core;
    running_.store(true);
    TaskHandle_t task = nullptr;
    if (xTaskCreate(ResponseTaskEntry, "esp_claw_rx", 6 * 1024, this, 4,
                    &task) != pdPASS) {
        running_.store(false);
        claw_core_stop(core, 2000);
        claw_core_destroy(core);
        core_ = nullptr;
        NotifyError("Beaver 响应任务创建失败");
        return false;
    }
    response_task_.store(task);
    ESP_LOGI(TAG, "Started backend=%s model=%s base_url=%s", backend_.c_str(),
             model_.c_str(), base_url_.c_str());
    return true;
#endif
}

void EspClawService::Stop() {
#if CONFIG_ESP_CLAW_AGENT
    request_inflight_.store(false);
    cancel_requested_.store(false);
    if (s_chat_coding_ready) {
        char output[256] = {};
        const esp_err_t stop_error =
            cap_lua_stop_all_jobs(nullptr, 2000, output, sizeof(output));
        if (stop_error != ESP_OK && stop_error != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to stop Lua jobs: %s (%s)",
                     esp_err_to_name(stop_error), output);
        }
        StopEspClawGameUi();
    }
    if (core_ == nullptr) {
        running_.store(false);
        return;
    }

    running_.store(false);
    for (int i = 0; response_task_.load() != nullptr && i < 20; ++i) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    auto core = static_cast<claw_core_handle_t>(core_);
    esp_err_t err = claw_core_stop(core, 5000);
    if (err == ESP_OK) {
        claw_core_destroy(core);
        core_ = nullptr;
    } else {
        ESP_LOGE(TAG, "Failed to stop core safely: %s", esp_err_to_name(err));
    }
#endif
}

bool EspClawService::Submit(const std::string& text,
                            const std::string& session_id) {
#if !CONFIG_ESP_CLAW_AGENT
    (void)text;
    (void)session_id;
    return false;
#else
    if (text.empty()) {
        return false;
    }
    if (!Start()) {
        return false;
    }
    if (text == "/session new" || text == "/session reset") {
        if (!s_chat_coding_ready) {
            NotifyError("SD 卡会话存储不可用，无法重置 Beaver 会话");
            return false;
        }
        ScheduleConversationReset();
        ApplyPendingConversationReset();
        if (on_response_) {
            on_response_("已开始新的 Beaver 会话，长期记忆仍然保留。");
        }
        return true;
    }

    // The speech WebSocket session changes on reconnect. ESP-Claw uses a
    // stable on-device session so conversation history survives reconnects.
    (void)session_id;
    const std::string session = kDefaultSession;
    claw_core_request_t request = {};
    request.request_id = next_request_id_.fetch_add(1);
    request.session_id = session.c_str();
    request.user_text = text.c_str();
    request.source_channel = "metalio_voice";
    request.target_channel = "metalio_display";
    const uint32_t now = xTaskGetTickCount();
    request_start_tick_.store(now);
    last_progress_tick_.store(now);
    last_progress_phase_.store(-1);
    tool_step_.store(0);
    cancel_requested_.store(false);
    request_inflight_.store(true);
    NotifyProgress("已提交 0s");
    const esp_err_t err = claw_core_submit(
        static_cast<claw_core_handle_t>(core_), &request, 1000);
    if (err != ESP_OK) {
        request_inflight_.store(false);
        NotifyError(std::string("Beaver 请求失败: ") + esp_err_to_name(err));
        return false;
    }
    return true;
#endif
}

bool EspClawService::CancelActiveRequest() {
#if !CONFIG_ESP_CLAW_AGENT
    return false;
#else
    if (!request_inflight_.load() || core_ == nullptr) {
        return false;
    }
    cancel_requested_.store(true);
    esp_err_t error = ESP_ERR_NOT_FOUND;
    // A very fast double tap can arrive after submit() has queued the request
    // but just before the core marks it in-flight. Give the agent loop a short
    // window to claim it so cancellation is deterministic in that boundary.
    for (int attempt = 0; attempt < 6 && request_inflight_.load(); ++attempt) {
        error = claw_core_cancel_request(
            static_cast<claw_core_handle_t>(core_), 0);
        if (error != ESP_ERR_NOT_FOUND) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (error != ESP_OK) {
        cancel_requested_.store(false);
        if (error != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to cancel active request: %s",
                     esp_err_to_name(error));
        }
        return false;
    }
    ESP_LOGI(TAG, "Active Beaver request cancellation armed");
    return true;
#endif
}

void EspClawService::ResponseTaskEntry(void* arg) {
    static_cast<EspClawService*>(arg)->ResponseTask();
}

void EspClawService::ResponseTask() {
#if CONFIG_ESP_CLAW_AGENT
    auto core = static_cast<claw_core_handle_t>(core_);
    while (running_.load()) {
        claw_core_response_t response = {};
        const esp_err_t err = claw_core_receive(core, &response, 250);
        if (err == ESP_ERR_TIMEOUT) {
            MaybeNotifyProgress();
            continue;
        }
        if (err != ESP_OK) {
            if (running_.load()) {
                request_inflight_.store(false);
                NotifyError(std::string("Beaver 响应失败: ") +
                            esp_err_to_name(err));
            }
            continue;
        }
        request_inflight_.store(false);
        const bool was_cancelled = cancel_requested_.exchange(false);
        if (was_cancelled) {
            NotifyError("request cancelled");
        } else if (response.status == CLAW_CORE_RESPONSE_STATUS_OK &&
                   response.text != nullptr) {
            if (on_response_) {
                on_response_(response.text);
            }
        } else {
            NotifyError(response.error_message != nullptr
                            ? response.error_message
                            : "Beaver 返回未知错误");
        }
        claw_core_response_free(&response);
        ApplyPendingConversationReset();
    }
#endif
    response_task_.store(nullptr);
    vTaskDelete(nullptr);
}

void EspClawService::ScheduleConversationReset() {
    reset_after_request_.store(true);
}

void EspClawService::ApplyPendingConversationReset() {
#if CONFIG_ESP_CLAW_AGENT
    if (!reset_after_request_.exchange(false) || !s_chat_coding_ready) {
        return;
    }
    bool history_deleted = false;
    bool skill_state_deleted = false;
    const esp_err_t history_error = claw_memory_delete_session_history(
        kDefaultSession, &history_deleted);
    const esp_err_t skill_error = claw_skill_delete_session_state(
        kDefaultSession, &skill_state_deleted);
    const esp_err_t visibility_error =
        claw_cap_set_session_llm_visible_groups(kDefaultSession, nullptr, 0);
    if (history_error != ESP_OK || skill_error != ESP_OK ||
        visibility_error != ESP_OK) {
        ESP_LOGE(TAG,
                 "Conversation reset failed: history=%s skill=%s visibility=%s",
                 esp_err_to_name(history_error), esp_err_to_name(skill_error),
                 esp_err_to_name(visibility_error));
        return;
    }
    ESP_LOGI(TAG,
             "Conversation reset complete: history=%d active_skills=%d; long-term memory preserved",
             history_deleted, skill_state_deleted);
#endif
}

namespace {

const char* ToolProgressLabel(const char* tool_name) {
    if (tool_name == nullptr) {
        return "执行";
    }
    if (std::strcmp(tool_name, "write_file") == 0) {
        return "写代码";
    }
    if (std::strcmp(tool_name, "read_file") == 0) {
        return "读代码";
    }
    if (std::strcmp(tool_name, "list_dir") == 0) {
        return "查文件";
    }
    if (std::strcmp(tool_name, "lua_run_script_async") == 0 ||
        std::strcmp(tool_name, "lua_run_script") == 0) {
        return "启动游戏";
    }
    if (std::strcmp(tool_name, "lua_stop_async_job") == 0 ||
        std::strcmp(tool_name, "lua_stop_all_async_jobs") == 0) {
        return "停止游戏";
    }
    if (std::strcmp(tool_name, "get_device_status") == 0) {
        return "读状态";
    }
    if (std::strcmp(tool_name, "get_recording_storage") == 0) {
        return "查录制";
    }
    if (std::strcmp(tool_name, "set_background_recording") == 0) {
        return "改录制";
    }
    if (std::strcmp(tool_name, "list_media_files") == 0) {
        return "查媒体";
    }
    if (std::strcmp(tool_name, "delete_media_files") == 0) {
        return "删媒体";
    }
    if (std::strcmp(tool_name, "activate_skill") == 0) {
        return "加载技能";
    }
    if (std::strncmp(tool_name, "memory_", 7) == 0) {
        return "访问记忆";
    }
    if (std::strcmp(tool_name, "register_skill") == 0) {
        return "安装技能";
    }
    if (std::strcmp(tool_name, "reset_conversation") == 0) {
        return "新建会话";
    }
    return "执行";
}

}  // namespace

void EspClawService::ReportToolStarted(const char* tool_name) {
    if (!request_inflight_.load() || cancel_requested_.load()) {
        return;
    }
    last_progress_phase_.store(
        static_cast<int>(CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL));
    const uint32_t step = tool_step_.fetch_add(1) + 1;
    char progress[96];
    std::snprintf(progress, sizeof(progress), "%s · 步骤%lu",
                  ToolProgressLabel(tool_name),
                  static_cast<unsigned long>(step));
    NotifyProgress(progress);
}

void EspClawService::ReportToolFinished(const char* tool_name, int result) {
    if (!request_inflight_.load() || cancel_requested_.load()) {
        return;
    }
    char progress[96];
    if (result == ESP_OK) {
        std::snprintf(progress, sizeof(progress), "%s完成",
                      ToolProgressLabel(tool_name));
    } else {
        std::snprintf(progress, sizeof(progress), "%s失败 · %s",
                      ToolProgressLabel(tool_name),
                      esp_err_to_name(static_cast<esp_err_t>(result)));
    }
    NotifyProgress(progress);
}

void EspClawService::MaybeNotifyProgress() {
#if CONFIG_ESP_CLAW_AGENT
    if (!request_inflight_.load() || cancel_requested_.load() ||
        core_ == nullptr) {
        return;
    }
    const uint32_t now = xTaskGetTickCount();
    const auto phase = claw_core_get_agent_loop_phase(
        static_cast<claw_core_handle_t>(core_));
    const int phase_value = static_cast<int>(phase);
    const bool phase_changed = last_progress_phase_.exchange(phase_value) !=
                               phase_value;
    const uint32_t heartbeat_ticks = pdMS_TO_TICKS(5000);
    const bool heartbeat_due =
        static_cast<uint32_t>(now - last_progress_tick_.load()) >=
        heartbeat_ticks;
    if (!phase_changed && !heartbeat_due) {
        return;
    }

    const uint32_t elapsed_seconds =
        static_cast<uint32_t>(now - request_start_tick_.load()) /
        configTICK_RATE_HZ;
    const uint32_t steps = tool_step_.load();
    const char* stage = "排队";
    switch (phase) {
        case CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT:
        case CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT:
            stage = "准备";
            break;
        case CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_LLM_HTTP:
        case CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP:
            stage = steps == 0 ? "分析" : "检查";
            break;
        case CLAW_CORE_AGENT_LOOP_PHASE_AFTER_LLM_BEFORE_TOOL:
            stage = "准备操作";
            break;
        case CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL:
            stage = "执行";
            break;
        case CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING:
            stage = "整理";
            break;
        case CLAW_CORE_AGENT_LOOP_PHASE_IDLE:
        default:
            break;
    }
    char progress[96];
    if (steps > 0) {
        std::snprintf(progress, sizeof(progress), "%s%lu · %lus",
                      stage, static_cast<unsigned long>(steps),
                      static_cast<unsigned long>(elapsed_seconds));
    } else {
        std::snprintf(progress, sizeof(progress), "%s · %lus", stage,
                      static_cast<unsigned long>(elapsed_seconds));
    }
    NotifyProgress(progress);
#endif
}

void EspClawService::NotifyProgress(const std::string& message) {
    last_progress_tick_.store(xTaskGetTickCount());
    ESP_LOGI(TAG, "Progress: %s", message.c_str());
    if (on_progress_) {
        on_progress_(message);
    }
}

void EspClawService::NotifyError(const std::string& message) const {
    ESP_LOGE(TAG, "%s", message.c_str());
    if (on_error_) {
        on_error_(message);
    }
}

std::string EspClawService::GetConfigSummary() const {
    return std::string("configured=") + (IsConfigured() ? "true" : "false") +
           ", running=" + (IsRunning() ? "true" : "false") +
           ", backend=" + backend_ + ", model=" + model_ +
           ", base_url=" + base_url_;
}
