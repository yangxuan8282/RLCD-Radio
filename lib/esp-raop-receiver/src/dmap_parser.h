#ifndef DMAP_PARSER_H
#define DMAP_PARSER_H

#include <stdint.h>
#include <stddef.h>

enum {
    TAG_UNKNOWN = 0,
    TAG_BYTE,
    TAG_SHORT,
    TAG_INT,
    TAG_LONG,
    TAG_STRING,
    TAG_DATE,
    TAG_VERSION,
    TAG_LIST,
    TAG_DATA
};

typedef struct {
    void *ctx;
    void (*on_int8)(void *ctx, uint8_t value);
    void (*on_int16)(void *ctx, uint16_t value);
    void (*on_int32)(void *ctx, uint32_t value);
    void (*on_int64)(void *ctx, uint64_t value);
    void (*on_string)(void *ctx, const char *code, const char *name, const char *buf, size_t len);
    void (*on_date)(void *ctx, uint32_t value);
    void (*on_version)(void *ctx, uint16_t major, uint8_t minor, uint8_t patch);
    void (*on_data)(void *ctx, unsigned char *data, size_t len);
    void (*on_unknown)(void *ctx, const char *tag);
} dmap_settings;

int dmap_parse(dmap_settings *settings, void *data, int len);

#endif // DMAP_PARSER_H