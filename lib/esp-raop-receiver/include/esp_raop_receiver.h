#ifndef ESP_RAOP_RECEIVER_H
#define ESP_RAOP_RECEIVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RAOP receiver handle
 */
typedef struct raop_handle_s raop_handle_t;

/**
 * @brief Volume control mode
 */
typedef enum {
    RAOP_VOLUME_SOFTWARE,  ///< Library applies volume scaling to PCM data
    RAOP_VOLUME_HARDWARE   ///< Consumer handles volume control via callback
} raop_volume_mode_t;

/**
 * @brief mDNS management mode
 */
typedef enum {
    RAOP_MDNS_MANAGED,   ///< Library initializes mDNS and advertises service
    RAOP_MDNS_EXTERNAL   ///< Consumer manages mDNS, library only adds service
} raop_mdns_mode_t;

/**
 * @brief RAOP event types
 */
typedef enum {
    RAOP_EVENT_CONNECTED,     ///< Client connected, session starting
    RAOP_EVENT_DISCONNECTED,  ///< Client disconnected, session ended
    RAOP_EVENT_BUFFERING,     ///< Audio data buffering before playback
    RAOP_EVENT_PLAYING,       ///< Audio playback started
    RAOP_EVENT_STOPPED,       ///< Playback stopped
    RAOP_EVENT_PAUSED,        ///< Playback paused
    RAOP_EVENT_VOLUME,        ///< Volume changed (hardware mode only)
    RAOP_EVENT_METADATA,      ///< Track metadata received
    RAOP_EVENT_ARTWORK,       ///< Album artwork received
    RAOP_EVENT_PROGRESS,      ///< Playback progress update
    RAOP_EVENT_STALLED,       ///< Stream stalled (network issues)
} raop_event_t;

/**
 * @brief Metadata information
 */
typedef struct {
    const char *artist;
    const char *album;
    const char *title;
} raop_metadata_t;

/**
 * @brief Artwork data
 */
typedef struct {
    const uint8_t *data;
    size_t len;
} raop_artwork_t;

/**
 * @brief Progress information
 */
typedef struct {
    uint32_t current_ms;
    uint32_t total_ms;
} raop_progress_t;

/**
 * @brief Audio output callback
 *
 * Called when PCM audio data is ready to be written to hardware.
 * In SOFTWARE volume mode, audio is pre-scaled.
 * In HARDWARE volume mode, audio is at full scale.
 *
 * @param data PCM audio data (16-bit stereo, 44.1kHz)
 * @param len Length in bytes
 * @param user_ctx User context pointer from config
 */
typedef void (*raop_audio_output_cb_t)(
    const uint8_t *data,
    size_t len,
    void *user_ctx
);

/**
 * @brief Volume change callback (HARDWARE mode only)
 *
 * @param volume Volume level (0.0 to 1.0)
 * @param user_ctx User context pointer from config
 */
typedef void (*raop_volume_cb_t)(
    float volume,
    void *user_ctx
);

/**
 * @brief Event callback
 *
 * @param event Event type
 * @param event_data Event-specific data (cast based on event type)
 * @param user_ctx User context pointer from config
 */
typedef void (*raop_event_cb_t)(
    raop_event_t event,
    void *event_data,
    void *user_ctx
);

/**
 * @brief PCM tap callback — called for every decoded audio frame before buffering.
 *        Provides raw PCM for external processing (e.g. FFT/visualisation).
 *        Must not block.
 *
 * @param data     PCM audio data (16-bit stereo interleaved, 44.1kHz)
 * @param len      Length in bytes
 * @param playtime When this frame is scheduled to play (ms, from esp_timer)
 * @param user_ctx User context pointer from config
 */
typedef void (*raop_pcm_tap_cb_t)(
    const uint8_t *data,
    size_t len,
    void *user_ctx
);

/**
 * @brief RAOP receiver configuration
 */
typedef struct {
    // Device identification (NULL/0 = auto-detect)
    const char *device_name;        ///< NULL = "ESP-XXXXXX" from MAC
    const uint8_t *mac_address;     ///< NULL = auto-detect from efuse
    uint32_t ip_address;            ///< 0 = auto-detect from default netif

    // Audio configuration
    uint32_t latency_frames;        ///< 0 = default (88200 frames = 2s @ 44.1kHz)
    raop_volume_mode_t volume_mode; ///< SOFTWARE or HARDWARE volume control

    // mDNS configuration
    raop_mdns_mode_t mdns_mode;     ///< MANAGED (default) or EXTERNAL
    const char *mdns_hostname;      ///< NULL = auto-generate from device_name

    // Required callbacks
    raop_audio_output_cb_t audio_output_cb;  ///< Audio output (required)

    // Optional callbacks
    raop_event_cb_t event_cb;                ///< Event notifications (optional)
    raop_volume_cb_t volume_cb;              ///< Volume control (required if HARDWARE mode)
    raop_pcm_tap_cb_t pcm_tap_cb;            ///< PCM tap for visualisation (optional, must not block)

    // User context
    void *user_ctx;  ///< Passed to all callbacks
} raop_config_t;

/**
 * @brief RAOP-specific error codes
 */
#define ESP_ERR_RAOP_BASE              0x7000
#define ESP_ERR_RAOP_NO_MEMORY         (ESP_ERR_RAOP_BASE + 1)  ///< Buffer allocation failed
#define ESP_ERR_RAOP_INVALID_CONFIG    (ESP_ERR_RAOP_BASE + 2)  ///< Invalid configuration
#define ESP_ERR_RAOP_NETWORK_FAILED    (ESP_ERR_RAOP_BASE + 3)  ///< Network setup failed
#define ESP_ERR_RAOP_CODEC_FAILED      (ESP_ERR_RAOP_BASE + 4)  ///< ALAC codec init failed
#define ESP_ERR_RAOP_MDNS_FAILED       (ESP_ERR_RAOP_BASE + 5)  ///< mDNS registration failed
#define ESP_ERR_RAOP_ALREADY_INIT      (ESP_ERR_RAOP_BASE + 6)  ///< Already initialized
#define ESP_ERR_RAOP_NOT_INIT          (ESP_ERR_RAOP_BASE + 7)  ///< Not initialized

/**
 * @brief Initialize RAOP receiver
 */
esp_err_t raop_init(const raop_config_t *config, raop_handle_t **out_handle);

/**
 * @brief Deinitialize RAOP receiver
 */
esp_err_t raop_deinit(raop_handle_t *handle);

/**
 * @brief Get current device name
 */
const char *raop_get_device_name(raop_handle_t *handle);

/**
 * @brief Set device name (updates mDNS advertisement)
 */
esp_err_t raop_set_device_name(raop_handle_t *handle, const char *name);

/**
 * @brief Get current volume level
 */
float raop_get_volume(raop_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif // ESP_RAOP_RECEIVER_H