#include "audio_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "audio_buffer";

#define BUFFER_FRAMES 512
#define MAX_FRAME_SIZE 2048
#define TIMING_THRESHOLD_MS 50  // Max drift before correction
#define TAP_LOOKAHEAD_MS    150  // Fire PCM tap this many ms before playtime

typedef struct {
    uint8_t data[MAX_FRAME_SIZE];
    size_t len;
    uint32_t playtime;
    bool ready;
    bool tapped;    // true once the lookahead tap has fired for this frame
} audio_frame_t;

static struct {
    audio_frame_t *frames;
    uint32_t read_idx;
    uint32_t write_idx;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    bool running;
    uint32_t start_time;
    audio_output_callback_t output_cb;
    void *user_ctx;
    // Optional PCM tap — called ~TAP_LOOKAHEAD_MS before playtime
    audio_pcm_tap_cb_t tap_cb;
    void *tap_user_ctx;
} audio_buf = {0};

static struct {
    uint32_t skip_frames;
    uint32_t pause_frames;
} timing_correction = {0, 0};

static uint32_t gettime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void audio_output_task(void *arg) {
    ESP_LOGD(TAG, "Audio output task started");

    while (audio_buf.running) {
        // Wait until we have a start time
        if (audio_buf.start_time == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Handle skip frames
        if (timing_correction.skip_frames > 0) {
            xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

            while (timing_correction.skip_frames > 0 && audio_buf.read_idx != audio_buf.write_idx) {
                audio_buf.frames[audio_buf.read_idx].ready = false;
                audio_buf.frames[audio_buf.read_idx].tapped = false;
                audio_buf.read_idx = (audio_buf.read_idx + 1) % BUFFER_FRAMES;
                timing_correction.skip_frames--;
            }

            xSemaphoreGive(audio_buf.mutex);
            continue;
        }

        // Handle pause frames (insert silence)
        if (timing_correction.pause_frames > 0) {
            uint8_t silence[MAX_FRAME_SIZE];
            memset(silence, 0, 1408); // 352 samples * 4 bytes
            audio_buf.output_cb(silence, 1408, audio_buf.user_ctx);
            timing_correction.pause_frames--;
            continue;
        }

        // Normal playback
        audio_frame_t *frame = NULL;

        xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);
        if (audio_buf.read_idx != audio_buf.write_idx) {
            frame = &audio_buf.frames[audio_buf.read_idx];
            if (frame->ready) {
                uint32_t now = gettime_ms();

                // Fire the PCM tap ~TAP_LOOKAHEAD_MS before playtime.
                // This gives the FFT pipeline time to process and send
                // the UDP packet so visualisation aligns with audio.
                if (audio_buf.tap_cb && !frame->tapped &&
                    frame->playtime > 0 && now >= frame->playtime - TAP_LOOKAHEAD_MS) {
                    frame->tapped = true;
                    // Copy data before releasing mutex — tap must not block
                    uint8_t tap_data[MAX_FRAME_SIZE];
                    memcpy(tap_data, frame->data, frame->len);
                    size_t tap_len = frame->len;
                    xSemaphoreGive(audio_buf.mutex);
                    audio_buf.tap_cb(tap_data, tap_len, audio_buf.tap_user_ctx);
                    // Re-acquire and re-check — frame may have been skipped/flushed
                    xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);
                    frame = &audio_buf.frames[audio_buf.read_idx];
                    if (!frame->ready) {
                        xSemaphoreGive(audio_buf.mutex);
                        continue;
                    }
                    now = gettime_ms();
                }

                // Check if it's time to play this frame
                if (now < frame->playtime) {
                    // Too early, wait
                    xSemaphoreGive(audio_buf.mutex);
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }

                uint8_t data[MAX_FRAME_SIZE];
                memcpy(data, frame->data, frame->len);
                size_t len = frame->len;

                frame->ready = false;
                frame->tapped = false;
                audio_buf.read_idx = (audio_buf.read_idx + 1) % BUFFER_FRAMES;

                xSemaphoreGive(audio_buf.mutex);

                audio_buf.output_cb(data, len, audio_buf.user_ctx);
            } else {
                xSemaphoreGive(audio_buf.mutex);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            xSemaphoreGive(audio_buf.mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGD(TAG, "Audio output task exiting");
    vTaskSuspend(NULL);
}

void audio_buffer_init(audio_output_callback_t output_cb, void *user_ctx,
                       audio_pcm_tap_cb_t tap_cb) {
    // If already initialized, just return
    if (audio_buf.frames && audio_buf.mutex) {
        ESP_LOGD(TAG, "Audio buffer already initialized");
        return;
    }

    // Clean up any partial state
    if (audio_buf.frames) free(audio_buf.frames);
    if (audio_buf.mutex) vSemaphoreDelete(audio_buf.mutex);

    memset(&audio_buf, 0, sizeof(audio_buf));

    // Store callback
    audio_buf.output_cb = output_cb;
    audio_buf.user_ctx = user_ctx;
    audio_buf.tap_cb = tap_cb;

    // Allocate frame buffer in PSRAM
    audio_buf.frames = heap_caps_malloc(BUFFER_FRAMES * sizeof(audio_frame_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_buf.frames) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer in PSRAM");
        return;
    }

    memset(audio_buf.frames, 0, BUFFER_FRAMES * sizeof(audio_frame_t));

    audio_buf.mutex = xSemaphoreCreateMutex();
    if (!audio_buf.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(audio_buf.frames);
        audio_buf.frames = NULL;
        return;
    }

    ESP_LOGI(TAG, "Audio buffer initialized");
}

void audio_buffer_start(void) {
    if (!audio_buf.frames || !audio_buf.mutex) {
        ESP_LOGE(TAG, "Cannot start: buffer not initialized");
        return;
    }

    if (audio_buf.running) {
        ESP_LOGW(TAG, "Audio buffer already running");
        return;
    }

    audio_buf.running = true;
    audio_buf.read_idx = 0;
    audio_buf.write_idx = 0;
    audio_buf.start_time = 0;

    xTaskCreatePinnedToCore(
        audio_output_task,
        "audio_output",
        4096,
        NULL,
        3,
        &audio_buf.task,
        1
    );

    ESP_LOGI(TAG, "Audio playback started");
}

void audio_buffer_set_start_time(uint32_t playtime) {
    if (!audio_buf.running) {
        ESP_LOGW(TAG, "Cannot set start time: buffer not running");
        return;
    }

    audio_buf.start_time = playtime;
    ESP_LOGD(TAG, "Start time set to %u ms", playtime);
}

void audio_buffer_stop(void) {
    if (!audio_buf.running) {
        ESP_LOGD(TAG, "Audio buffer already stopped");
        return;
    }

    audio_buf.running = false;

    if (audio_buf.task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(audio_buf.task);
        audio_buf.task = NULL;
    }

    ESP_LOGI(TAG, "Audio playback stopped");
}

bool audio_buffer_write(const uint8_t *data, size_t len, uint32_t playtime) {
    if (!audio_buf.frames || !audio_buf.mutex) {
        ESP_LOGW(TAG, "Buffer not initialized, dropping frame");
        return false;
    }

    if (len > MAX_FRAME_SIZE) {
        ESP_LOGE(TAG, "Frame too large: %zu bytes", len);
        return false;
    }

    // Try up to 5 times with 10ms delays between attempts
    for (int retries = 0; retries < 5; retries++) {
        xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

        uint32_t next_write = (audio_buf.write_idx + 1) % BUFFER_FRAMES;

        // Check if buffer has space
        if (next_write != audio_buf.read_idx) {
            audio_frame_t *frame = &audio_buf.frames[audio_buf.write_idx];
            memcpy(frame->data, data, len);
            frame->len = len;
            frame->playtime = playtime;
            frame->ready = true;
            frame->tapped = false;

            audio_buf.write_idx = next_write;

            xSemaphoreGive(audio_buf.mutex);
            return true;
        }

        xSemaphoreGive(audio_buf.mutex);

        // Buffer full, wait for I2S to drain
        if (retries < 4) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGW(TAG, "Buffer full after retries, dropping frame");
    return false;
}

void audio_buffer_flush(void) {
    xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

    // Mark all frames as not ready
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        audio_buf.frames[i].ready = false;
        audio_buf.frames[i].tapped = false;
    }

    audio_buf.read_idx = 0;
    audio_buf.write_idx = 0;

    xSemaphoreGive(audio_buf.mutex);

    ESP_LOGD(TAG, "Buffer flushed");
}

void audio_buffer_deinit(void) {
    // Stop playback if running
    if (audio_buf.running) {
        audio_buffer_stop();
    }

    if (audio_buf.mutex) {
        vSemaphoreDelete(audio_buf.mutex);
        audio_buf.mutex = NULL;
    }

    if (audio_buf.frames) {
        free(audio_buf.frames);
        audio_buf.frames = NULL;
    }

    ESP_LOGI(TAG, "Audio buffer deinitialized");
}

void audio_buffer_get_timing(uint32_t *frames_buffered, uint32_t *head_playtime) {

    if (!audio_buf.frames || !audio_buf.mutex) {
        *frames_buffered = 0;
        *head_playtime = 0;
        return;
    }

    xSemaphoreTake(audio_buf.mutex, portMAX_DELAY);

    // Calculate how many frames are buffered
    if (audio_buf.write_idx >= audio_buf.read_idx) {
        *frames_buffered = audio_buf.write_idx - audio_buf.read_idx;
    } else {
        *frames_buffered = BUFFER_FRAMES - audio_buf.read_idx + audio_buf.write_idx;
    }

    // Get the playtime of the most recent frame
    if (*frames_buffered > 0) {
        uint32_t head_idx = (audio_buf.write_idx == 0) ? BUFFER_FRAMES - 1 : audio_buf.write_idx - 1;
        *head_playtime = audio_buf.frames[head_idx].playtime;
    } else {
        *head_playtime = 0;
    }

    xSemaphoreGive(audio_buf.mutex);
}

void audio_buffer_skip_frames(uint32_t count) {
    timing_correction.skip_frames = count;
    ESP_LOGD(TAG, "Will skip %u frames", count);
}

void audio_buffer_pause_frames(uint32_t count) {
    timing_correction.pause_frames = count;
    ESP_LOGD(TAG, "Will pause %u frames", count);
}

bool audio_buffer_is_ready(void) {
    return audio_buf.frames != NULL && audio_buf.running;
}