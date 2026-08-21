#ifndef LOG_UTIL_H
#define LOG_UTIL_H

#include "esp_log.h"

typedef enum { lSILENCE = 0, lERROR, lWARN, lINFO, lDEBUG, lSDEBUG } log_level;

#define LOG_ERROR(fmt, ...) ESP_LOGE("raop", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ESP_LOGW("raop", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ESP_LOGI("raop", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) ESP_LOGD("raop", fmt, ##__VA_ARGS__)
#define LOG_SDEBUG(fmt, ...) ESP_LOGV("raop", fmt, ##__VA_ARGS__)

#endif // LOG_UTIL_H