#include "esp_raop_receiver.h"
#include "raop.h"
#include "audio_buffer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdarg.h>

static const char *TAG = "raop_core";
#define MAX_FRAME_SIZE 2048

// The opaque handle consumers get back from raop_init()
struct raop_handle_s {
    struct raop_ctx_s *raop_ctx;
    raop_config_t config;
    char device_name[64];
    float volume;
};

// ---- Internal callback: translates internal events to public API ----

static bool internal_cmd_cb(raop_internal_event_t event, ...) {
    // Retrieve handle via a static pointer
    // This is safe because we only support one instance
    extern raop_handle_t *s_handle;
    raop_handle_t *handle = s_handle;
    if (!handle) return false;

    va_list args;
    va_start(args, event);

    switch (event) {

        // ---- Internally managed events ----

        case RAOP_INT_SETUP: {
            uint8_t **buffer = va_arg(args, uint8_t**);
            size_t *size = va_arg(args, size_t*);

            *size = 352 * 4 * 512;
            *buffer = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!*buffer) {
                ESP_LOGE(TAG, "Failed to allocate RTP buffer");
                *size = 0;
            }

            audio_buffer_init(handle->config.audio_output_cb, handle->config.user_ctx,
                  handle->config.pcm_tap_cb);

            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_CONNECTED, NULL, handle->config.user_ctx);
            }
            break;
        }

        case RAOP_INT_STREAM:
            audio_buffer_start();
            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_BUFFERING, NULL, handle->config.user_ctx);
            }
            break;

        case RAOP_INT_PLAY: {
            uint32_t playtime = va_arg(args, uint32_t);
            audio_buffer_set_start_time(playtime);
            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_PLAYING, NULL, handle->config.user_ctx);
            }
            break;
        }

        case RAOP_INT_STOP:
            audio_buffer_stop();
            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_STOPPED, NULL, handle->config.user_ctx);
            }
            break;

        case RAOP_INT_FLUSH:
            audio_buffer_flush();
            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_PAUSED, NULL, handle->config.user_ctx);
            }
            break;

        case RAOP_INT_TEARDOWN:
            audio_buffer_deinit();
            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_DISCONNECTED, NULL, handle->config.user_ctx);
            }
            break;

        case RAOP_INT_TIMING: {
            if (!audio_buffer_is_ready()) break;

            uint32_t frames_buffered, head_playtime;
            audio_buffer_get_timing(&frames_buffered, &head_playtime);
            if (frames_buffered == 0) break;

            uint32_t now = gettime_ms();
            uint32_t buffer_duration_ms = (frames_buffered * 352 * 1000) / 44100;
            uint32_t local_head_time = now + buffer_duration_ms;
            int32_t error = (int32_t)(head_playtime - local_head_time);

            if (error < -50) {
                uint32_t skip = (-error * 44100) / (352 * 1000);
                audio_buffer_skip_frames(skip);
            } else if (error > 50) {
                uint32_t pause = (error * 44100) / (352 * 1000);
                audio_buffer_pause_frames(pause);
            }
            break;
        }

        // ---- Consumer-facing events ----

        case RAOP_INT_VOLUME: {
            float volume = (float)va_arg(args, double);
            handle->volume = volume;

            if (handle->config.volume_mode == RAOP_VOLUME_SOFTWARE) {
                // Will be applied in audio output path - store for use there
            } else if (handle->config.volume_cb) {
                handle->config.volume_cb(volume, handle->config.user_ctx);
            }

            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_VOLUME, &volume, handle->config.user_ctx);
            }
            break;
        }

        case RAOP_INT_METADATA: {
            if (handle->config.event_cb) {
                raop_metadata_t meta = {
                    .artist = va_arg(args, char*),
                    .album  = va_arg(args, char*),
                    .title  = va_arg(args, char*),
                };
                va_arg(args, uint32_t); // consume timestamp
                handle->config.event_cb(RAOP_EVENT_METADATA, &meta, handle->config.user_ctx);
            }
            break;
        }

        case RAOP_INT_ARTWORK: {
            if (handle->config.event_cb) {
                raop_artwork_t artwork = {
                    .data = va_arg(args, uint8_t*),
                    .len  = va_arg(args, size_t),
                };
                va_arg(args, uint32_t); // consume timestamp
                handle->config.event_cb(RAOP_EVENT_ARTWORK, &artwork, handle->config.user_ctx);
            }
            break;
        }

        case RAOP_INT_PROGRESS: {
            if (handle->config.event_cb) {
                raop_progress_t progress = {
                    .current_ms = va_arg(args, int),
                    .total_ms   = va_arg(args, int),
                };
                handle->config.event_cb(RAOP_EVENT_PROGRESS, &progress, handle->config.user_ctx);
            }
            break;
        }

        case RAOP_INT_STALLED:
            if (handle->config.event_cb) {
                handle->config.event_cb(RAOP_EVENT_STALLED, NULL, handle->config.user_ctx);
            }
            break;

        default:
            break;
    }

    va_end(args);
    return true;
}

// ---- Single instance support ----
raop_handle_t *s_handle = NULL;

static void apply_software_volume(const uint8_t *data, size_t len, uint8_t *out, float volume) {
    const int16_t *samples = (const int16_t *)data;
    int16_t *out_samples = (int16_t *)out;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        int32_t scaled = (int32_t)(samples[i] * volume);
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        out_samples[i] = (int16_t)scaled;
    }
}

static void internal_data_cb(const uint8_t *data, size_t len, uint32_t playtime) {
    if (s_handle && s_handle->config.volume_mode == RAOP_VOLUME_SOFTWARE
            && s_handle->volume < 0.99f) {
        static uint8_t scaled[MAX_FRAME_SIZE];
        apply_software_volume(data, len, scaled, s_handle->volume);
        if (!audio_buffer_write(scaled, len, playtime)) {
            ESP_LOGW(TAG, "Failed to buffer audio frame");
        }
    } else {
        if (!audio_buffer_write(data, len, playtime)) {
            ESP_LOGW(TAG, "Failed to buffer audio frame");
        }
    }
}

// ---- Public API implementation ----

esp_err_t raop_init(const raop_config_t *config, raop_handle_t **out_handle) {
    if (!config || !out_handle) return ESP_ERR_INVALID_ARG;
    if (!config->audio_output_cb) return ESP_ERR_RAOP_INVALID_CONFIG;
    if (config->volume_mode == RAOP_VOLUME_HARDWARE && !config->volume_cb) {
        return ESP_ERR_RAOP_INVALID_CONFIG;
    }
    if (s_handle) return ESP_ERR_RAOP_ALREADY_INIT;

    raop_handle_t *handle = calloc(1, sizeof(raop_handle_t));
    if (!handle) return ESP_ERR_NO_MEM;

    // Copy config
    memcpy(&handle->config, config, sizeof(raop_config_t));
    handle->volume = 1.0f;

    // Resolve device name
    uint8_t mac[6];
    if (config->mac_address) {
        memcpy(mac, config->mac_address, 6);
    } else {
        esp_efuse_mac_get_default(mac);
    }

    if (config->device_name) {
        snprintf(handle->device_name, sizeof(handle->device_name), "%s", config->device_name);
    } else {
        snprintf(handle->device_name, sizeof(handle->device_name),
                 "ESP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    // Resolve IP
    uint32_t ip = config->ip_address;
    if (ip == 0) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                ip = ip_info.ip.addr;
            }
        }
    }

    if (ip == 0) {
        ESP_LOGE(TAG, "Failed to resolve IP address");
        free(handle);
        return ESP_ERR_RAOP_NETWORK_FAILED;
    }

    // mDNS setup
    if (config->mdns_mode == RAOP_MDNS_MANAGED) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
            free(handle);
            return ESP_ERR_RAOP_MDNS_FAILED;
        }

        const char *hostname = config->mdns_hostname ? config->mdns_hostname : handle->device_name;
        mdns_hostname_set(hostname);
        mdns_instance_name_set(handle->device_name);
        ESP_LOGI(TAG, "mDNS hostname: %s.local", hostname);
    }

    // Create internal RAOP context
    s_handle = handle;
    handle->raop_ctx = raop_create(ip, handle->device_name, mac,
                               config->latency_frames ? config->latency_frames : 88200,
                               internal_cmd_cb, internal_data_cb,
                               config->mdns_mode);

    if (!handle->raop_ctx) {
        s_handle = NULL;
        free(handle);
        return ESP_ERR_RAOP_NETWORK_FAILED;
    }

    ESP_LOGI(TAG, "RAOP receiver started: %s", handle->device_name);
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t raop_deinit(raop_handle_t *handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle != s_handle) return ESP_ERR_RAOP_NOT_INIT;

    audio_buffer_deinit();

    if (handle->raop_ctx) {
        raop_delete(handle->raop_ctx);
        handle->raop_ctx = NULL;
    }

    if (handle->config.mdns_mode == RAOP_MDNS_MANAGED) {
        mdns_free();
    }

    s_handle = NULL;
    free(handle);
    return ESP_OK;
}

const char *raop_get_device_name(raop_handle_t *handle) {
    if (!handle) return NULL;
    return handle->device_name;
}

esp_err_t raop_set_device_name(raop_handle_t *handle, const char *name) {
    if (!handle || !name) return ESP_ERR_INVALID_ARG;
    snprintf(handle->device_name, sizeof(handle->device_name), "%s", name);
    return ESP_OK;
}

float raop_get_volume(raop_handle_t *handle) {
    if (!handle) return 0.0f;
    return handle->volume;
}