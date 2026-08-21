/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef RAOP_SINK_H
#define RAOP_SINK_H

#include <stdint.h>
#include <stdarg.h>

#define RAOP_SAMPLE_RATE	44100

typedef enum {  RAOP_INT_SETUP, RAOP_INT_STREAM, RAOP_INT_PLAY, RAOP_INT_FLUSH, RAOP_INT_METADATA, RAOP_INT_ARTWORK, RAOP_INT_PROGRESS, RAOP_INT_PAUSE, RAOP_INT_STOP, RAOP_INT_STALLED,
                                RAOP_INT_TIMING, RAOP_INT_PREV, RAOP_INT_NEXT, RAOP_INT_REW, RAOP_INT_FWD,
                                RAOP_INT_VOLUME, RAOP_INT_VOLUME_UP, RAOP_INT_VOLUME_DOWN, RAOP_INT_RESUME, RAOP_INT_TOGGLE,
                                RAOP_INT_TEARDOWN } raop_internal_event_t ;

typedef bool (*raop_cmd_cb_t)(raop_internal_event_t event, ...);
typedef bool (*raop_cmd_vcb_t)(raop_internal_event_t event, va_list args);
typedef void (*raop_data_cb_t)(const uint8_t *data, size_t len, uint32_t playtime);

/**
 * @brief     init sink mode (need to be provided)
 */
void raop_sink_init(raop_cmd_vcb_t cmd_cb, raop_data_cb_t data_cb);

/**
 * @brief     deinit sink mode (need to be provided)
 */
void raop_sink_deinit(void);

/**
 * @brief     force disconnection
 */
void raop_disconnect(void);

#endif /* RAOP_SINK_H*/