#ifndef RAOP_H
#define RAOP_H

#include <stdbool.h>
#include <stdint.h>
#include "raop_sink.h"
#include "util.h"
#include "esp_raop_receiver.h"

struct raop_ctx_s;

struct raop_ctx_s *raop_create(uint32_t host, char *name,
                               unsigned char mac[6], int latency,
                               raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb,
                               raop_mdns_mode_t mdns_mode);

void raop_delete(struct raop_ctx_s *ctx);
void raop_abort(struct raop_ctx_s *ctx);
bool raop_cmd(struct raop_ctx_s *ctx, raop_internal_event_t event, void *param);

#endif // RAOP_H