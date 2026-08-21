#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define NFREE(p) if (p) { free(p); p = NULL; }

// Delayed free for FreeRTOS TCB cleanup
#define SAFE_PTR_FREE(P)							\
	do {											\
		TimerHandle_t timer = xTimerCreate("cleanup", pdMS_TO_TICKS(10000), pdFALSE, P, _delayed_free);	\
		xTimerStart(timer, portMAX_DELAY);			\
	} while (0)

static inline void _delayed_free(TimerHandle_t xTimer) {
	free(pvTimerGetTimerID(xTimer));
	xTimerDelete(xTimer, portMAX_DELAY);
}

typedef struct {
    char *key;
    char *data;
} key_data_t;

struct metadata_s {
    char *artist;
    char *album;
    char *title;
    char *genre;
    char *path;
    char *artwork;
    char *remote_title;
};

// Network utilities
in_addr_t get_localhost(char **name);
int shutdown_socket(int sd);
int bind_socket(unsigned short *port, int mode);
int conn_socket(unsigned short port);
uint32_t gettime_ms(void);

// String utilities
char *strlwr(char *str);
char *strextract(char *s1, char *beg, char *end);

// HTTP utilities
bool http_parse(int sock, char *method, key_data_t *rkd, char **body, int *len);
char *http_send(int sock, char *method, key_data_t *rkd);

// Key-data utilities
char *kd_lookup(key_data_t *kd, char *key);
bool kd_add(key_data_t *kd, char *key, char *data);
void kd_free(key_data_t *kd);
char *kd_dump(key_data_t *kd);

// Metadata
void free_metadata(struct metadata_s *metadata);

#endif // UTIL_H