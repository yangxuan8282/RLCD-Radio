/*
 *  platform setting definition
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 */

#ifndef __PLATFORM_H
#define __PLATFORM_H

#define LINUX     1
#define WIN       0

#include <stdbool.h>
#include <signal.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <lwip/inet.h>
#include <pthread.h>
#include <errno.h>
#include "esp_timer.h"

#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

typedef int16_t   s16_t;
typedef int32_t   s32_t;
typedef int64_t   s64_t;
typedef uint8_t   u8_t;
typedef uint16_t  u16_t;
typedef uint32_t  u32_t;
typedef uint64_t  u64_t;

#define last_error() errno
#define ERROR_WOULDBLOCK EWOULDBLOCK

char *strlwr(char *str);
#define _random(x) random()
#define closesocket(s) close(s)
#define S_ADDR(X) X.s_addr

// Time utilities
uint32_t gettime_ms(void);

typedef struct ntp_s {
	u32_t seconds;
	u32_t fraction;
} ntp_t;

u64_t timeval_to_ntp(struct timeval tv, struct ntp_s *ntp);
u64_t get_ntp(struct ntp_s *ntp);

#endif     // __PLATFORM