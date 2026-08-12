#ifndef BACKGROUND_CAPTURE_SERVICE_H
#define BACKGROUND_CAPTURE_SERVICE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

class BackgroundCaptureService {
public:
    BackgroundCaptureService() = default;
    ~BackgroundCaptureService();

    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    static void AudioWriterTaskEntry(void* arg);
    static void MediaUploaderTaskEntry(void* arg);
    void AudioWriterTask();
    void MediaUploaderTask();
    void HandleAudioInput(const std::vector<int16_t>& pcm,
                          size_t requested_samples);
    bool SendAudio(const int16_t* samples, size_t sample_count);
    bool UploadMediaFile(const std::string& path, const char* content_type,
                         int& status_code);

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> audio_drops_{0};
    RingbufHandle_t audio_ringbuf_ = nullptr;
    TaskHandle_t audio_writer_task_ = nullptr;
    TaskHandle_t media_uploader_task_ = nullptr;
    bool ringbuf_with_caps_ = false;
    uint32_t boot_id_ = 0;
};

#endif
