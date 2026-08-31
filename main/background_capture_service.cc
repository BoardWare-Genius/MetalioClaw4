#include "background_capture_service.h"

#include "application.h"
#include "audio/audio_service.h"
#include "boards/common/board.h"
#include "boards/common/SdCardManager.hpp"
#include "display/screen/camera_screen/camera_screen.h"
#include "display/screen/digital_people_screen/digital_people_screen.h"
#include "settings.h"
#include "system_info.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>

namespace {

constexpr const char* TAG = "BgCapture";
constexpr const char* kAudioDir = "/sdcard/bgaudio";
constexpr size_t kAudioRingBytes = 256 * 1024;
constexpr uint32_t kSampleRate = 16000;
constexpr uint16_t kChannels = 1;
constexpr uint16_t kBitsPerSample = 16;
constexpr const char* kPhotoDir = "/sdcard/bgphoto";
constexpr const char* kSettingsNamespace = "bg_record";
constexpr const char* kAudioEnabledKey = "audio";
constexpr const char* kCameraEnabledKey = "camera";

void SetU16(std::array<uint8_t, 44>& out, size_t offset, uint16_t value) {
    out[offset] = static_cast<uint8_t>(value & 0xff);
    out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void SetU32(std::array<uint8_t, 44>& out, size_t offset, uint32_t value) {
    out[offset] = static_cast<uint8_t>(value & 0xff);
    out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

std::array<uint8_t, 44> MakeWavHeader(uint32_t data_size) {
    std::array<uint8_t, 44> out{};
    std::memcpy(out.data(), "RIFF", 4);
    SetU32(out, 4, 36 + data_size);
    std::memcpy(out.data() + 8, "WAVEfmt ", 8);
    SetU32(out, 16, 16);
    SetU16(out, 20, 1);
    SetU16(out, 22, kChannels);
    SetU32(out, 24, kSampleRate);
    SetU32(out, 28, kSampleRate * kChannels * kBitsPerSample / 8);
    SetU16(out, 32, kChannels * kBitsPerSample / 8);
    SetU16(out, 34, kBitsPerSample);
    std::memcpy(out.data() + 36, "data", 4);
    SetU32(out, 40, data_size);
    return out;
}

bool EnsureDirectory(const char* path) {
    struct stat st = {};
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

bool HasSuffix(const std::string& value, const char* suffix) {
    const size_t suffix_size = std::strlen(suffix);
    return value.size() >= suffix_size &&
           value.compare(value.size() - suffix_size, suffix_size, suffix) == 0;
}

bool FindNextMediaFile(const char* directory, const char* prefix,
                       const char* suffix,
                       std::string& path) {
    DIR* dir = opendir(directory);
    if (dir == nullptr) {
        return false;
    }
    std::string selected;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." ||
            name.rfind(prefix, 0) != 0 || !HasSuffix(name, suffix)) {
            continue;
        }
        if (selected.empty() || name < selected) {
            selected = name;
        }
    }
    closedir(dir);
    if (selected.empty()) {
        return false;
    }
    path = std::string(directory) + "/" + selected;
    return true;
}

std::string Basename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string PaddedDecimal(uint64_t value, size_t width) {
    std::string text = std::to_string(value);
    if (text.size() < width) {
        text.insert(0, width - text.size(), '0');
    }
    return text;
}

std::string BuildUploadFilename(const char* prefix, const char* extension,
                                int64_t monotonic_ms,
                                uint64_t sequence) {
    return std::string(prefix) + "_u" + std::to_string(monotonic_ms) + "_" +
           PaddedDecimal(sequence, 6) + "." + extension;
}

std::string Sha256File(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    auto* buffer = static_cast<unsigned char*>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t buffer_size = 4096;
    if (buffer == nullptr) {
        buffer_size = 1024;
        buffer = static_cast<unsigned char*>(heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT));
    }
    if (buffer == nullptr) {
        fclose(file);
        return {};
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    while (ok) {
        const size_t count = fread(buffer, 1, buffer_size, file);
        if (count > 0 && mbedtls_sha256_update(&context, buffer, count) != 0) {
            ok = false;
        }
        if (count < buffer_size) {
            if (ferror(file)) {
                ok = false;
            }
            break;
        }
    }
    fclose(file);
    heap_caps_free(buffer);

    unsigned char digest[32] = {};
    if (ok) {
        ok = mbedtls_sha256_finish(&context, digest) == 0;
    }
    mbedtls_sha256_free(&context);
    if (!ok) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        result[i * 2] = kHex[digest[i] >> 4];
        result[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return result;
}

struct MediaIdentity {
    std::string device_id;
    std::string boot_id;
    std::string stream_id;
    std::string filename;
    std::string sha256;
    uint64_t sequence = 0;
    int64_t monotonic_ms = 0;
    int64_t captured_at_ms = 0;
    uint32_t duration_ms = 0;
    bool clock_synced = false;
};

bool BuildMediaIdentity(const std::string& path, const char* content_type,
                        size_t content_length,
                        MediaIdentity& identity) {
    const std::string basename = Basename(path);
    const bool is_audio = std::strcmp(content_type, "audio/wav") == 0;
    unsigned long parsed_boot = 0;
    unsigned long parsed_uptime = 0;
    unsigned long parsed_sequence = 0;
    const char* pattern = is_audio ? "audio_%8lx_%lu_%lu.wav" :
                                     "photo_%8lx_%lu_%lu.jpg";
    if (std::sscanf(basename.c_str(), pattern, &parsed_boot, &parsed_uptime,
                    &parsed_sequence) != 3) {
        ESP_LOGW(TAG, "Unsupported queued media filename: %s", basename.c_str());
        return false;
    }

    char boot_id[16];
    std::snprintf(boot_id, sizeof(boot_id), "%08lx", parsed_boot);
    std::string compact_mac = SystemInfo::GetMacAddress();
    compact_mac.erase(std::remove(compact_mac.begin(), compact_mac.end(), ':'), compact_mac.end());
    identity.device_id = std::string(BOARD_NAME) + "-" + compact_mac;
    identity.boot_id = boot_id;
    identity.stream_id = is_audio ? "microphone-main" : "camera-main";
    identity.sequence = parsed_sequence;
    identity.monotonic_ms = parsed_uptime;
    // The existing OTA path stores local wall time directly in the ESP Unix
    // clock. That value looks valid but is shifted by the timezone offset.
    // Declare it unsynchronised and let the gateway anchor this monotonic
    // timestamp to its authoritative server receive time instead.
    identity.clock_synced = false;
    identity.captured_at_ms = 0;
    if (is_audio && content_length > 44) {
        identity.duration_ms = static_cast<uint32_t>(
            (content_length - 44) * 1000 / (kSampleRate * sizeof(int16_t)));
    }
    identity.filename = BuildUploadFilename(
        is_audio ? "audio" : "image", is_audio ? "wav" : "jpg",
        identity.monotonic_ms, identity.sequence);
    identity.sha256 = Sha256File(path);
    return !identity.sha256.empty();
}

}  // namespace

BackgroundCaptureService::BackgroundCaptureService() {
    Settings settings(kSettingsNamespace);
    bool audio_enabled = settings.GetBool(kAudioEnabledKey, true);
    bool camera_enabled = settings.GetBool(kCameraEnabledKey, true);
    // A corrupt or manually edited NVS state must not make the record control
    // start an apparently active session that captures nothing.
    if (!audio_enabled && !camera_enabled) {
        audio_enabled = true;
        camera_enabled = true;
    }
    audio_enabled_.store(audio_enabled);
    camera_enabled_.store(camera_enabled);
}

BackgroundCaptureService::~BackgroundCaptureService() { Stop(); }

bool BackgroundCaptureService::SetSources(bool audio_enabled,
                                          bool camera_enabled) {
    if (!audio_enabled && !camera_enabled) {
        return false;
    }
    audio_enabled_.store(audio_enabled);
    camera_enabled_.store(camera_enabled);
    Settings settings(kSettingsNamespace, true);
    settings.SetBool(kAudioEnabledKey, audio_enabled);
    settings.SetBool(kCameraEnabledKey, camera_enabled);
    if (!running_.load()) {
        DigitalPeopleScreen::SetBackgroundRecordingState(
            false, false, audio_enabled, camera_enabled);
    }
    return true;
}

bool BackgroundCaptureService::Start() {
#if CONFIG_BACKGROUND_RECORDING
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }

    const bool want_audio = audio_enabled_.load();
    const bool want_camera = camera_enabled_.load();
    if (!want_audio && !want_camera) {
        running_.store(false);
        return false;
    }
    do {
        boot_id_ = esp_random();
    } while (boot_id_ == 0);

    if (want_audio) {
        audio_ringbuf_ = xRingbufferCreateWithCaps(
            kAudioRingBytes, RINGBUF_TYPE_NOSPLIT,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ringbuf_with_caps_ = audio_ringbuf_ != nullptr;
        if (audio_ringbuf_ == nullptr) {
            audio_ringbuf_ = xRingbufferCreate(kAudioRingBytes,
                                               RINGBUF_TYPE_NOSPLIT);
        }
        if (audio_ringbuf_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate background audio ring buffer");
        } else {
            Application::GetInstance().GetAudioService().SetAudioInputTapCallback(
                [this](const std::vector<int16_t>& pcm,
                       size_t requested_samples) {
                    HandleAudioInput(pcm, requested_samples);
                });
            if (xTaskCreatePinnedToCore(
                    AudioWriterTaskEntry, "bg_audio_writer", 8 * 1024, this,
                    tskIDLE_PRIORITY + 2, &audio_writer_task_, 0) == pdPASS) {
                audio_active_.store(true);
                Application::GetInstance().GetAudioService().EnableContinuousInput(true);
            } else {
                ESP_LOGE(TAG, "Failed to create background audio writer task");
                Application::GetInstance().GetAudioService().SetAudioInputTapCallback(nullptr);
                if (ringbuf_with_caps_) {
                    vRingbufferDeleteWithCaps(audio_ringbuf_);
                } else {
                    vRingbufferDelete(audio_ringbuf_);
                }
                audio_ringbuf_ = nullptr;
            }
        }
    }

    if (want_camera) {
        if (CameraScreen::StartBackgroundCapture() == ESP_OK) {
            CameraScreen::EnableBackgroundRecording(
                true, CONFIG_BACKGROUND_RECORDING_PHOTO_INTERVAL_MS, boot_id_);
            camera_active_.store(true);
        } else {
            ESP_LOGE(TAG, "Failed to start camera for background recording");
        }
    }

    if (!audio_active_.load() && !camera_active_.load()) {
        running_.store(false);
        DigitalPeopleScreen::SetBackgroundRecordingState(
            false, false, want_audio, want_camera);
        return false;
    }

    if (CONFIG_BACKGROUND_RECORDING_UPLOAD_URL[0] != '\0' &&
        xTaskCreatePinnedToCore(MediaUploaderTaskEntry, "bg_media_upload",
                                10 * 1024, this, tskIDLE_PRIORITY + 1,
                                &media_uploader_task_, 0) != pdPASS) {
        media_uploader_task_ = nullptr;
        ESP_LOGW(TAG, "Failed to create background media uploader task");
    }
    const bool sd_ready = SdCardManager::GetInstance().IsMounted();
    DigitalPeopleScreen::SetBackgroundRecordingState(
        true, sd_ready, audio_active_.load(), camera_active_.load());
    ESP_LOGI(TAG,
             "Continuous SD recording started: audio=%d camera=%d "
             "photo_ms=%d audio_segment_ms=%d sd=%d",
             audio_active_.load() ? 1 : 0, camera_active_.load() ? 1 : 0,
             CONFIG_BACKGROUND_RECORDING_PHOTO_INTERVAL_MS,
             CONFIG_BACKGROUND_RECORDING_AUDIO_SEGMENT_MS, sd_ready ? 1 : 0);
    return true;
#else
    return false;
#endif
}

void BackgroundCaptureService::Stop() {
#if CONFIG_BACKGROUND_RECORDING
    if (!running_.exchange(false)) {
        return;
    }
    if (audio_active_.exchange(false)) {
        Application::GetInstance().GetAudioService().SetAudioInputTapCallback(nullptr);
        Application::GetInstance().GetAudioService().EnableContinuousInput(false);
    }
    if (camera_active_.exchange(false)) {
        CameraScreen::EnableBackgroundRecording(false, 0, 0);
        CameraScreen::StopBackgroundCapture();
    }
    // Let the writer close and publish its last partial WAV before the uploader
    // drains this recording session.  The toggle runs on its own task, so this
    // orderly shutdown never blocks LVGL or the power-key monitor.
    while (audio_writer_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    DigitalPeopleScreen::SetBackgroundRecordingState(
        false, false, audio_enabled_.load(), camera_enabled_.load());
    while (media_uploader_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (audio_ringbuf_ != nullptr) {
        if (ringbuf_with_caps_) {
            vRingbufferDeleteWithCaps(audio_ringbuf_);
        } else {
            vRingbufferDelete(audio_ringbuf_);
        }
        audio_ringbuf_ = nullptr;
    }
#endif
}

void BackgroundCaptureService::AudioWriterTaskEntry(void* arg) {
    auto* self = static_cast<BackgroundCaptureService*>(arg);
    self->AudioWriterTask();
    self->audio_writer_task_ = nullptr;
    vTaskDelete(nullptr);
}

void BackgroundCaptureService::MediaUploaderTaskEntry(void* arg) {
    auto* self = static_cast<BackgroundCaptureService*>(arg);
    self->MediaUploaderTask();
    self->media_uploader_task_ = nullptr;
    vTaskDelete(nullptr);
}

bool BackgroundCaptureService::SendAudio(const int16_t* samples,
                                         size_t sample_count) {
    if (audio_ringbuf_ == nullptr || samples == nullptr || sample_count == 0) {
        return false;
    }
    if (xRingbufferSend(audio_ringbuf_, samples,
                        sample_count * sizeof(int16_t), 0) == pdTRUE) {
        return true;
    }
    const uint32_t drops = audio_drops_.fetch_add(1) + 1;
    if (drops == 1 || drops % 100 == 0) {
        ESP_LOGW(TAG, "Background audio ring full, dropped chunks=%lu",
                 static_cast<unsigned long>(drops));
    }
    return false;
}

void BackgroundCaptureService::HandleAudioInput(
    const std::vector<int16_t>& pcm, size_t requested_samples) {
    if (!running_.load() || pcm.empty() || requested_samples == 0 ||
        Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        return;
    }

    if (pcm.size() == requested_samples) {
        SendAudio(pcm.data(), pcm.size());
        return;
    }

    const size_t channels = pcm.size() / requested_samples;
    if (channels > 1 && pcm.size() == requested_samples * channels) {
        std::vector<int16_t> mono(requested_samples);
        for (size_t i = 0; i < requested_samples; ++i) {
            mono[i] = pcm[i * channels];
        }
        SendAudio(mono.data(), mono.size());
        return;
    }

    SendAudio(pcm.data(), std::min(pcm.size(), requested_samples));
}

void BackgroundCaptureService::AudioWriterTask() {
    const uint32_t target_samples = std::max<uint32_t>(
        1, CONFIG_BACKGROUND_RECORDING_AUDIO_SEGMENT_MS * kSampleRate / 1000);
    FILE* file = nullptr;
    std::string temp_path;
    std::string final_path;
    uint32_t segment_samples = 0;
    uint32_t sequence = 0;

    auto close_segment = [&](bool publish) {
        if (file == nullptr) {
            return;
        }
        bool ok = publish && segment_samples > 0;
        if (ok) {
            const auto header = MakeWavHeader(segment_samples * sizeof(int16_t));
            ok = fseek(file, 0, SEEK_SET) == 0 &&
                 fwrite(header.data(), 1, header.size(), file) == header.size() &&
                 fflush(file) == 0 && fsync(fileno(file)) == 0;
        }
        if (fclose(file) != 0) {
            ok = false;
        }
        file = nullptr;
        if (ok && rename(temp_path.c_str(), final_path.c_str()) == 0) {
            ESP_LOGI(TAG, "Saved background audio: %s samples=%lu drops=%lu",
                     final_path.c_str(), static_cast<unsigned long>(segment_samples),
                     static_cast<unsigned long>(audio_drops_.load()));
        } else {
            unlink(temp_path.c_str());
            if (publish) {
                ESP_LOGW(TAG, "Failed to finalize background audio: %s",
                         final_path.c_str());
            }
        }
        temp_path.clear();
        final_path.clear();
        segment_samples = 0;
    };

    auto open_segment = [&]() -> bool {
        if (!SdCardManager::GetInstance().IsMounted() ||
            !EnsureDirectory(kAudioDir)) {
            DigitalPeopleScreen::SetBackgroundRecordingState(
                true, false, audio_active_.load(), camera_active_.load());
            return false;
        }
        char path[128];
        const unsigned long uptime_ms =
            static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
        std::snprintf(path, sizeof(path), "%s/audio_%08lx_%08lu_%04lu.wav",
                      kAudioDir, static_cast<unsigned long>(boot_id_), uptime_ms,
                      static_cast<unsigned long>(sequence++));
        final_path = path;
        temp_path = final_path + ".tmp";
        file = fopen(temp_path.c_str(), "wb+");
        if (file == nullptr) {
            ESP_LOGW(TAG, "Open background WAV failed: %s errno=%d %s",
                     temp_path.c_str(), errno, strerror(errno));
            return false;
        }
        const auto header = MakeWavHeader(0);
        if (fwrite(header.data(), 1, header.size(), file) != header.size()) {
            close_segment(false);
            return false;
        }
        segment_samples = 0;
        DigitalPeopleScreen::SetBackgroundRecordingState(
            true, true, audio_active_.load(), camera_active_.load());
        return true;
    };

    ESP_LOGI(TAG, "Background audio writer started");
    while (running_.load()) {
        size_t item_size = 0;
        auto* item = static_cast<uint8_t*>(xRingbufferReceive(
            audio_ringbuf_, &item_size, pdMS_TO_TICKS(500)));
        if (item == nullptr) {
            continue;
        }
        if (!SdCardManager::GetInstance().IsMounted()) {
            close_segment(false);
            DigitalPeopleScreen::SetBackgroundRecordingState(
                true, false, audio_active_.load(), camera_active_.load());
            vRingbufferReturnItem(audio_ringbuf_, item);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (file == nullptr && !open_segment()) {
            vRingbufferReturnItem(audio_ringbuf_, item);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t offset = 0;
        while (offset < item_size && file != nullptr) {
            const size_t remaining_bytes =
                (target_samples - segment_samples) * sizeof(int16_t);
            const size_t write_bytes = std::min(item_size - offset, remaining_bytes);
            if (fwrite(item + offset, 1, write_bytes, file) != write_bytes) {
                ESP_LOGW(TAG, "Write background WAV failed: errno=%d %s", errno,
                         strerror(errno));
                close_segment(false);
                break;
            }
            offset += write_bytes;
            segment_samples += write_bytes / sizeof(int16_t);
            if (segment_samples >= target_samples) {
                close_segment(true);
                if (offset < item_size && !open_segment()) {
                    break;
                }
            }
        }
        vRingbufferReturnItem(audio_ringbuf_, item);
    }
    close_segment(segment_samples > 0);
    ESP_LOGI(TAG, "Background audio writer stopped");
}

bool BackgroundCaptureService::UploadMediaFile(const std::string& path,
                                               const char* content_type,
                                               int& status_code) {
    status_code = 0;
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "Cannot stat media for upload: %s errno=%d %s",
                 path.c_str(), errno, strerror(errno));
        return false;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Cannot open media for upload: %s errno=%d %s",
                 path.c_str(), errno, strerror(errno));
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        fclose(file);
        return false;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        fclose(file);
        return false;
    }

    MediaIdentity identity;
    if (!BuildMediaIdentity(path, content_type,
                            static_cast<size_t>(st.st_size), identity)) {
        fclose(file);
        return false;
    }

    http->SetTimeout(CONFIG_BACKGROUND_RECORDING_UPLOAD_TIMEOUT_MS);
    http->SetKeepAlive(false);
    http->SetHeader("Content-Type", "application/octet-stream");
    http->SetHeader("Content-Length", std::to_string(st.st_size));
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", identity.device_id);
    http->SetHeader("X-Boot-Id", identity.boot_id);
    http->SetHeader("X-Stream-Id", identity.stream_id);
    http->SetHeader("X-Sequence", std::to_string(identity.sequence));
    http->SetHeader("X-Monotonic-Ms", std::to_string(identity.monotonic_ms));
    http->SetHeader("X-Clock-Synced", identity.clock_synced ? "true" : "false");
    if (identity.clock_synced) {
        http->SetHeader("X-Captured-At-Ms", std::to_string(identity.captured_at_ms));
    }
    http->SetHeader("X-Duration-Ms", std::to_string(identity.duration_ms));
    http->SetHeader("X-Filename", identity.filename);
    http->SetHeader("X-Media-Type", content_type);
    http->SetHeader("X-Content-Length", std::to_string(st.st_size));
    http->SetHeader("X-Content-Sha256", identity.sha256);
    http->SetHeader("Authorization",
                    "Bearer " CONFIG_BACKGROUND_RECORDING_UPLOAD_TOKEN);
    // This HttpClient otherwise enables chunk framing whenever Open() is
    // followed by Write(), even when Content-Length was supplied explicitly.
    // An engaged, empty content value selects fixed-length streaming without
    // buffering the complete 960 KB WAV in memory.
    http->SetContent(std::string{});
    if (!http->Open("POST", CONFIG_BACKGROUND_RECORDING_UPLOAD_URL)) {
        fclose(file);
        http->Close();
        return false;
    }

    auto write_all = [&http](const char* data, size_t size) {
        size_t written = 0;
        while (written < size) {
            const int count = http->Write(data + written, size - written);
            if (count <= 0) {
                return false;
            }
            written += static_cast<size_t>(count);
        }
        return true;
    };

    bool ok = true;
    std::array<char, 4096> buffer{};
    while (ok) {
        const size_t read_size = fread(buffer.data(), 1, buffer.size(), file);
        if (read_size > 0 && !write_all(buffer.data(), read_size)) {
            ok = false;
            break;
        }
        if (read_size < buffer.size()) {
            if (ferror(file)) {
                ok = false;
            }
            break;
        }
    }
    fclose(file);
    std::string response_body;
    if (ok) {
        status_code = http->GetStatusCode();
        if (status_code > 0) {
            response_body = http->ReadAll();
        }
    }
    http->Close();

    bool acknowledged = false;
    std::string acknowledgement;
    if (!response_body.empty()) {
        cJSON* root = cJSON_ParseWithLength(response_body.data(), response_body.size());
        cJSON* status = root == nullptr ? nullptr : cJSON_GetObjectItem(root, "status");
        if (cJSON_IsString(status) && status->valuestring != nullptr) {
            acknowledgement = status->valuestring;
            acknowledged = (status_code == 201 && acknowledgement == "accepted") ||
                           (status_code == 200 && acknowledgement == "already_accepted");
        }
        cJSON_Delete(root);
    }
    ok = ok && acknowledged;
    if (!ok && !response_body.empty()) {
        const int response_log_length = static_cast<int>(
            std::min<size_t>(response_body.size(), 512));
        ESP_LOGW(TAG, "Beaver media rejection body: %.*s",
                 response_log_length, response_body.c_str());
    }
    ESP_LOGI(TAG,
             "Beaver media upload: file=%s stream=%s sequence=%lu bytes=%lu "
             "status=%d ack=%s result=%s",
             identity.filename.c_str(), identity.stream_id.c_str(),
             static_cast<unsigned long>(identity.sequence),
             static_cast<unsigned long>(st.st_size), status_code,
             acknowledgement.empty() ? "missing" : acknowledgement.c_str(),
             ok ? "ok" : "failed");
    return ok;
}

void BackgroundCaptureService::MediaUploaderTask() {
    ESP_LOGI(TAG, "Background media uploader started: %s",
             CONFIG_BACKGROUND_RECORDING_UPLOAD_URL);
    char audio_prefix[32];
    char photo_prefix[32];
    std::snprintf(audio_prefix, sizeof(audio_prefix), "audio_%08lx_",
                  static_cast<unsigned long>(boot_id_));
    std::snprintf(photo_prefix, sizeof(photo_prefix), "photo_%08lx_",
                  static_cast<unsigned long>(boot_id_));
    // Old boots and previously rejected media stay recoverable on the SD
    // card, but must never overtake the live stream and fill the server's
    // expensive capsule-processing queue.
    ESP_LOGI(TAG,
             "Live upload queue selected: boot=%08lx clock=server-anchored",
             static_cast<unsigned long>(boot_id_));
    uint32_t failures = 0;
    bool audio_first = true;
    while (true) {
        if (!SdCardManager::GetInstance().IsMounted()) {
            if (!running_.load() && audio_writer_task_ == nullptr) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::string path;
        bool found = false;
        if (audio_first) {
            found = FindNextMediaFile(kAudioDir, audio_prefix, ".wav", path) ||
                    FindNextMediaFile(kPhotoDir, photo_prefix, ".jpg", path);
        } else {
            found = FindNextMediaFile(kPhotoDir, photo_prefix, ".jpg", path) ||
                    FindNextMediaFile(kAudioDir, audio_prefix, ".wav", path);
        }
        audio_first = !audio_first;
        if (!found) {
            // Stop() first waits for AudioWriterTask to publish its last
            // partial WAV.  Only exit after that file has appeared and the
            // current boot queue has been drained.
            if (!running_.load() && audio_writer_task_ == nullptr) {
                break;
            }
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const char* content_type = HasSuffix(path, ".wav") ? "audio/wav" : "image/jpeg";
        int status_code = 0;
        if (UploadMediaFile(path, content_type, status_code)) {
            if (unlink(path.c_str()) == 0) {
                ESP_LOGI(TAG, "Uploaded and removed SD media: %s", path.c_str());
            } else {
                ESP_LOGW(TAG, "Uploaded but failed to remove SD media: %s errno=%d %s",
                         path.c_str(), errno, strerror(errno));
            }
            failures = 0;
            taskYIELD();
            continue;
        }

        if (status_code >= 400 && status_code < 500 && status_code != 401 &&
            status_code != 403 && status_code != 408 && status_code != 429) {
            const std::string rejected = path + ".rejected";
            if (rename(path.c_str(), rejected.c_str()) == 0) {
                ESP_LOGE(TAG, "Server permanently rejected media; retained as %s",
                         rejected.c_str());
            }
            failures = 0;
            continue;
        }

        failures++;
        if (!running_.load()) {
            ESP_LOGW(TAG,
                     "Recording stopped with media retained for retry: %s",
                     path.c_str());
            break;
        }
        const uint32_t shift = std::min<uint32_t>(failures - 1, 5);
        const uint32_t base_ms = std::min<uint32_t>(1000U << shift, 30000U);
        const uint32_t delay_ms = base_ms + esp_random() % std::max<uint32_t>(1, base_ms / 5);
        ESP_LOGW(TAG, "Media upload retry in %lu ms, queued=%s failures=%lu",
                 static_cast<unsigned long>(delay_ms), path.c_str(),
                 static_cast<unsigned long>(failures));
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    ESP_LOGI(TAG, "Background media uploader stopped");
}
