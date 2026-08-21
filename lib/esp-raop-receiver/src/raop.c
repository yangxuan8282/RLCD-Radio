/*
 *
 * (c) Philippe 2019, philippe_44@outlook.com
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 */

#include <stdio.h>
#include "platform.h"
#include "mdns.h"
#include "mbedtls/version.h"
#include <mbedtls/x509.h>
#include <mbedtls/aes.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#include "util.h"
#include "raop.h"
#include "rtp.h"
#include "dmap_parser.h"
#include "log_util.h"

#define RTSP_STACK_SIZE 	(24*1024)
#define SEARCH_STACK_SIZE	(3*1024)

typedef struct raop_ctx_s {
	struct in_addr host;	// IP of bridge
	short unsigned port;    // RTSP port for RAOP
	int sock;               // socket of the above
	struct in_addr peer;	// IP of the iDevice (RAOP sender)
	bool running;
	TaskHandle_t thread, joiner;
	StaticTask_t *xTaskBuffer;
	StackType_t xStack[RTSP_STACK_SIZE] __attribute__ ((aligned (4)));
	bool abort;
	unsigned char mac[6];
	int latency;
	struct {
		char *aesiv, *aeskey;
		char *fmtp;
	} rtsp;
	struct rtp_s *rtp;
	raop_cmd_cb_t	cmd_cb;
	raop_data_cb_t	data_cb;
	struct {
		char				DACPid[32], id[32];
		struct in_addr		host;
		u16_t				port;
		bool running;
		TaskHandle_t thread;
		StaticTask_t *xTaskBuffer;
		StackType_t xStack[SEARCH_STACK_SIZE] __attribute__ ((aligned (4)));;
		SemaphoreHandle_t destroy_mutex;
	} active_remote;
	void *owner;
} raop_ctx_t;

extern log_level	raop_loglevel;

static void		rtsp_thread(void *arg);
static void 	search_remote(void *args);
static void		cleanup_rtsp(raop_ctx_t *ctx, bool abort);
static bool 	handle_rtsp(raop_ctx_t *ctx, int sock);

static char*	rsa_apply(unsigned char *input, int inlen, int *outlen, int mode);
static int  	base64_pad(char *src, char **padded);
static int 		base64_encode(const void *data, int size, char **str);
static int 		base64_decode(const char *str, void *data);

enum { RSA_MODE_KEY, RSA_MODE_AUTH };

static void on_dmap_string(void *ctx, const char *code, const char *name, const char *buf, size_t len);

/*----------------------------------------------------------------------------*/
struct raop_ctx_s *raop_create(uint32_t host, char *name,
						unsigned char mac[6], int latency,
						raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb,
						raop_mdns_mode_t mdns_mode) {
	struct raop_ctx_s *ctx = heap_caps_malloc(sizeof(struct raop_ctx_s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	struct sockaddr_in addr;
	char id[64];

	const mdns_txt_item_t txt[] = {
		{"am", "airesp32"},
		{"tp", "UDP"},
		{"sm","false"},
		{"sv","false"},
		{"ek","1"},
		{"et","0,1"},
		{"md","0,1,2"},
		{"cn","0,1"},
		{"ch","2"},
		{"ss","16"},
		{"sr","44100"},
		{"vn","3"},
		{"txtvers","1"},
	};

	if (!ctx) return NULL;

	// make sure we have a clean context
	memset(ctx, 0, sizeof(raop_ctx_t));

	ctx->host.s_addr = host;
	ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
	ctx->cmd_cb = cmd_cb;
	ctx->data_cb = data_cb;
	ctx->latency = min(latency, 88200);

	if (ctx->sock == -1) {
		LOG_ERROR("Cannot create listening socket", NULL);
		free(ctx);
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = host;
	addr.sin_family = AF_INET;
	ctx->port = 5000;
	addr.sin_port = htons(ctx->port);

	if (bind(ctx->sock, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(ctx->sock, 1)) {
		LOG_ERROR("Cannot bind or listen RTSP listener: %s", strerror(errno));
		closesocket(ctx->sock);
		free(ctx);
		return NULL;
	}

	ctx->running = true;
	memcpy(ctx->mac, mac, 6);
	snprintf(id, 64, "%02X%02X%02X%02X%02X%02X@%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);

	// Only register mDNS if managed mode
	if (mdns_mode == RAOP_MDNS_MANAGED) {
		LOG_INFO("starting mDNS with %s", id);
		mdns_service_add(id, "_raop", "_tcp", ctx->port, (mdns_txt_item_t*) txt, sizeof(txt) / sizeof(mdns_txt_item_t));
	} else {
		LOG_INFO("mDNS external mode - service registration skipped for %s", id);
	}

  ctx->xTaskBuffer = (StaticTask_t*) heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	BaseType_t core_id = (CONFIG_PTHREAD_TASK_CORE_DEFAULT == -1) ? tskNO_AFFINITY : CONFIG_PTHREAD_TASK_CORE_DEFAULT;
	ctx->thread = xTaskCreateStaticPinnedToCore( (TaskFunction_t) rtsp_thread, "RTSP", RTSP_STACK_SIZE, ctx,
                                             ESP_TASK_PRIO_MIN + 2, ctx->xStack, ctx->xTaskBuffer,
                                             core_id);

	return ctx;
}

/*----------------------------------------------------------------------------*/
void raop_abort(struct raop_ctx_s *ctx) {
	LOG_INFO("[%p]: aborting RTSP session at next select() wakeup", ctx);
	ctx->abort = true;
}

/*----------------------------------------------------------------------------*/
void raop_delete(struct raop_ctx_s *ctx) {
	if (!ctx) return;

	// then the RTSP task
	ctx->joiner = xTaskGetCurrentTaskHandle();
	ctx->running = false;

	// brute-force exit of accept()
	shutdown(ctx->sock, SHUT_RDWR);
	closesocket(ctx->sock);

	// wait to make sure LWIP if scheduled (avoid issue with NotifyTake)
	vTaskDelay(100 / portTICK_PERIOD_MS);
	ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
	vTaskDelete(ctx->thread);
	if (ctx->xTaskBuffer) free(ctx->xTaskBuffer);

	// cleanup all session-created items
	cleanup_rtsp(ctx, true);

	mdns_service_remove("_raop", "_tcp");

	if (ctx->rtsp.aeskey) free(ctx->rtsp.aeskey);
	if (ctx->rtsp.aesiv) free(ctx->rtsp.aesiv);
	if (ctx->rtsp.fmtp) free(ctx->rtsp.fmtp);

	free(ctx);
}

/*----------------------------------------------------------------------------*/
bool raop_cmd(struct raop_ctx_s *ctx, raop_internal_event_t event, void *param) {
	struct sockaddr_in addr;
	int sock;
	char *command = NULL;
    bool success = false;

	// first notify the remote controller (if any)
	switch(event) {
		case RAOP_INT_REW:
			command = strdup("beginrew");
			break;
		case RAOP_INT_FWD:
			command = strdup("beginff");
			break;
		case RAOP_INT_PREV:
			command = strdup("previtem");
			break;
		case RAOP_INT_NEXT:
			command = strdup("nextitem");
			break;
		case RAOP_INT_TOGGLE:
			command = strdup("playpause");
			break;
		case RAOP_INT_PAUSE:
			command = strdup("pause");
			break;
		case RAOP_INT_PLAY:
			command = strdup("play");
			break;
		case RAOP_INT_RESUME:
			command = strdup("playresume");
			break;
		case RAOP_INT_STOP:
			command = strdup("stop");
			break;
		case RAOP_INT_VOLUME_UP:
			command = strdup("volumeup");
			break;
		case RAOP_INT_VOLUME_DOWN:
			command = strdup("volumedown");
			break;
		case RAOP_INT_VOLUME: {
			float Volume = *((float*) param);
			Volume = Volume ? (Volume - 1) * 30 : -144;
			asprintf(&command,"setproperty?dmcp.device-volume=%0.4lf", Volume);
			break;
		}
		default:
			break;
	}

	// no command to send to remote or no remote found yet
	if (!command || !ctx->active_remote.port) {
		if (command) free(command);
		return success;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = S_ADDR(ctx->active_remote.host);
	addr.sin_port = htons(ctx->active_remote.port);

	if (!connect(sock, (struct sockaddr*) &addr, sizeof(addr))) {
		char *method, *buf, resp[512] = "";
		int len;
		key_data_t headers[4] = { {NULL, NULL} };

		asprintf(&method, "GET /ctrl-int/1/%s HTTP/1.0", command);
		kd_add(headers, "Active-Remote", ctx->active_remote.id);
		kd_add(headers, "Connection", "close");

		buf = http_send(sock, method, headers);
		len = recv(sock, resp, 512, 0);
		if (len > 0) resp[len-1] = '\0';
		LOG_DEBUG("[%p]: sending remote\n%s<== received ==>\n%s", ctx, buf, resp);

		if (method) free(method);
		if (buf) free(buf);
		kd_free(headers);
        success = true;
	} else {
		LOG_INFO("[%p]: can't connect to remote for %s", ctx, command);
	}

	free(command);
	closesocket(sock);

	return success;
}

/*----------------------------------------------------------------------------*/
static void rtsp_thread(void *arg) {
	raop_ctx_t *ctx = (raop_ctx_t*) arg;
	int  sock = -1;

	while (ctx->running) {
		fd_set rfds;
		struct timeval timeout = {0, 100*1000};
		int n;
		bool res = false;

		if (sock == -1) {
			struct sockaddr_in peer;
			socklen_t addrlen = sizeof(struct sockaddr_in);

			sock = accept(ctx->sock, (struct sockaddr*) &peer, &addrlen);
			ctx->peer.s_addr = peer.sin_addr.s_addr;
			ctx->abort = false;

			if (sock != -1 && ctx->running) {
				LOG_INFO("got RTSP connection %u", sock);
			} else continue;
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		n = select(sock + 1, &rfds, NULL, NULL, &timeout);

		if (!n && !ctx->abort) continue;

		if (n > 0) res = handle_rtsp(ctx, sock);

		if (n < 0 || !res || ctx->abort) {
			cleanup_rtsp(ctx, true);
			closesocket(sock);
			LOG_INFO("RTSP close %u", sock);
			sock = -1;
		}
	}

	if (sock != -1) closesocket(sock);

	xTaskNotifyGive(ctx->joiner);
	vTaskSuspend(NULL);
}

/*----------------------------------------------------------------------------*/
static void apple_challenge(raop_ctx_t *ctx, int sock, key_data_t *req_headers, key_data_t *resp_headers) {
	char *hdr = kd_lookup(req_headers, "Apple-Challenge");
	if (!hdr)
		return;

	// try to re-acquire IP address if we were missing it
	if (S_ADDR(ctx->host) == INADDR_ANY) {
		S_ADDR(ctx->host) = get_localhost(NULL);
		LOG_INFO("[%p]: IP was missing, trying to get it %s", ctx, inet_ntoa(ctx->host));
	}

	char *buf_pad = NULL;

	// need to pad the base64 string as apple devices don't
	base64_pad(hdr, &buf_pad);

	char data[32];
	char *p = data + min(base64_decode(buf_pad, data), 32-10);
	p = (char*) memcpy(p, &S_ADDR(ctx->host), 4) + 4;
	p = (char*) memcpy(p, ctx->mac, 6) + 6;
	memset(p, 0, 32 - (p - data));

	char hex_dump[65];
	for (int j = 0; j < 32; j++) {
			sprintf(hex_dump + (j * 2), "%02x", (unsigned char)data[j]);
	}
	hex_dump[64] = '\0';
	int n;
	char *rsa_result = rsa_apply((unsigned char*) data, 32, &n, RSA_MODE_AUTH);

	char *data_b64 = NULL;
	base64_encode(rsa_result, n, &data_b64);

	// remove padding as well (seems to be optional now)
	for (n = strlen(data_b64) - 1; n > 0 && data_b64[n] == '='; data_b64[n--] = '\0');

	kd_add(resp_headers, "Apple-Response", data_b64);

	if (rsa_result) free(rsa_result);
	if (buf_pad) free(buf_pad);
	if (data_b64) free(data_b64);
}

/*----------------------------------------------------------------------------*/
static bool handle_rtsp(raop_ctx_t *ctx, int sock)
{
	char *buf = NULL, *body = NULL, method[16] = "";
	key_data_t headers[32], resp[16] = { {NULL, NULL} };
	int len;
	bool success = true;

	LOG_DEBUG("[%p]: received %s", ctx, method);

	if (!http_parse(sock, method, headers, &body, &len)) {
		if (body) free(body);
		kd_free(headers);
		return false;
	}

	// Handle Apple-Challenge
	apple_challenge(ctx, sock, headers, resp);

	// standard headers
	kd_add(resp, "CSeq", kd_lookup(headers, "CSeq"));
	kd_add(resp, "Server", "AirTunes/105.1");

	// method-specific headers
	if (!strcmp(method, "OPTIONS")) {

		kd_add(resp, "Public", "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");

	} else if (!strcmp(method, "ANNOUNCE")) {
		char *padded, *p;

		if (ctx->rtsp.aeskey) free(ctx->rtsp.aeskey);
		if (ctx->rtsp.aesiv) free(ctx->rtsp.aesiv);
		if (ctx->rtsp.fmtp) free(ctx->rtsp.fmtp);
		ctx->rtsp.aeskey = NULL;
		ctx->rtsp.aesiv = NULL;
		ctx->rtsp.fmtp = NULL;

		if ((p = strcasestr(body, "rsaaeskey")) != NULL) {
			unsigned char *aeskey;
			int len, outlen;

			p = strextract(p, ":", "\r\n");
			base64_pad(p, &padded);
			aeskey = malloc(strlen(padded));
			len = base64_decode(padded, aeskey);
			ctx->rtsp.aeskey = rsa_apply(aeskey, len, &outlen, RSA_MODE_KEY);

			free(p);
			free(aeskey);
			free(padded);
		}

		if ((p = strcasestr(body, "aesiv")) != NULL) {
			p = strextract(p, ":", "\r\n");
			base64_pad(p, &padded);
			ctx->rtsp.aesiv = malloc(strlen(padded));
			base64_decode(padded, ctx->rtsp.aesiv);

			free(p);
			free(padded);
		}

		if ((p = strcasestr(body, "fmtp")) != NULL) {
			p = strextract(p, ":", "\r\n");
			ctx->rtsp.fmtp = strdup(p);
			free(p);
		}

		// on announce, search remote
		if ((buf = kd_lookup(headers, "DACP-ID")) != NULL) strcpy(ctx->active_remote.DACPid, buf);
		if ((buf = kd_lookup(headers, "Active-Remote")) != NULL) strcpy(ctx->active_remote.id, buf);

		ctx->active_remote.running = true;
		ctx->active_remote.destroy_mutex = xSemaphoreCreateBinary();
		ctx->active_remote.xTaskBuffer = (StaticTask_t*) heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		ctx->active_remote.thread = xTaskCreateStaticPinnedToCore( (TaskFunction_t) search_remote, "search_remote", SEARCH_STACK_SIZE, ctx,
                                                            ESP_TASK_PRIO_MIN + 2, ctx->active_remote.xStack, ctx->active_remote.xTaskBuffer,
                                                            tskNO_AFFINITY );

	} else if (!strcmp(method, "SETUP") && ((buf = kd_lookup(headers, "Transport")) != NULL)) {
		char *p;
		rtp_resp_t rtp = { 0 };
		short unsigned tport = 0, cport = 0;
		uint8_t *buffer = NULL;
		size_t size = 0;

		success = ctx->cmd_cb(RAOP_INT_SETUP, &buffer, &size);

		if ((p = strcasestr(buf, "timing_port")) != NULL) sscanf(p, "%*[^=]=%hu", &tport);
		if ((p = strcasestr(buf, "control_port")) != NULL) sscanf(p, "%*[^=]=%hu", &cport);

		rtp = rtp_init(ctx->peer, ctx->latency, ctx->rtsp.aeskey, ctx->rtsp.aesiv,
					   ctx->rtsp.fmtp, cport, tport, buffer, size, ctx->cmd_cb, ctx->data_cb);

		ctx->rtp = rtp.ctx;

		if ( (cport * tport * rtp.cport * rtp.tport * rtp.aport) != 0 && rtp.ctx) {
			char *transport;
			asprintf(&transport, "RTP/AVP/UDP;unicast;mode=record;control_port=%u;timing_port=%u;server_port=%u", rtp.cport, rtp.tport, rtp.aport);
			kd_add(resp, "Transport", transport);
			kd_add(resp, "Session", "DEADBEEF");
			free(transport);
		} else {
			success = false;
			LOG_INFO("[%p]: cannot start session, missing ports", ctx);
		}

	} else if (!strcmp(method, "RECORD")) {
		unsigned short seqno = 0;
		unsigned rtptime = 0;
		char *p;

		if (ctx->latency) {
			char latency[6];
			snprintf(latency, 6, "%u", ctx->latency);
			kd_add(resp, "Audio-Latency", latency);
		}

		buf = kd_lookup(headers, "RTP-Info");
		if (buf && (p = strcasestr(buf, "seq")) != NULL) sscanf(p, "%*[^=]=%hu", &seqno);
		if (buf && (p = strcasestr(buf, "rtptime")) != NULL) sscanf(p, "%*[^=]=%u", &rtptime);

		if (ctx->rtp) rtp_record(ctx->rtp, seqno, rtptime);

		success = ctx->cmd_cb(RAOP_INT_STREAM);

	}  else if (!strcmp(method, "FLUSH")) {
		unsigned short seqno = 0;
		unsigned rtptime = 0;
		char *p;

		buf = kd_lookup(headers, "RTP-Info");
		if ((p = strcasestr(buf, "seq")) != NULL) sscanf(p, "%*[^=]=%hu", &seqno);
		if ((p = strcasestr(buf, "rtptime")) != NULL) sscanf(p, "%*[^=]=%u", &rtptime);

		if (ctx->rtp && rtp_flush(ctx->rtp, seqno, rtptime, true)) {
			success = ctx->cmd_cb(RAOP_INT_FLUSH);
			rtp_flush_release(ctx->rtp);
		}

	}  else if (!strcmp(method, "TEARDOWN")) {

		cleanup_rtsp(ctx, false);
		success = ctx->cmd_cb(RAOP_INT_STOP);

	} else if (!strcmp(method, "SET_PARAMETER")) {
		char *p;

		if (body && (p = strcasestr(body, "volume")) != NULL) {
			float volume;

			sscanf(p, "%*[^:]:%f", &volume);
			volume = (volume == -144.0) ? 0 : (1 + volume / 30);
			success = ctx->cmd_cb(RAOP_INT_VOLUME, volume);
		} else if (body && (p = strcasestr(body, "progress")) != NULL) {
			int start, current, stop = 0;

			sscanf(p, "%*[^:]:%u/%u/%u", &start, &current, &stop);
			current = ((current - start) / 44100) * 1000;
			if (stop) stop = ((stop - start) / 44100) * 1000;
			LOG_DEBUG("[%p]: SET PARAMETER progress %d/%u %s", ctx, current, stop, p);
			success = ctx->cmd_cb(RAOP_INT_PROGRESS, max(current, 0), stop);
		} else if (body && ((p = kd_lookup(headers, "Content-Type")) != NULL) && !strcasecmp(p, "application/x-dmap-tagged")) {
			struct metadata_s metadata;
			dmap_settings settings = {
					.ctx = &metadata,
					.on_string = on_dmap_string
			};

			settings.ctx = &metadata;
			memset(&metadata, 0, sizeof(struct metadata_s));
			if (!dmap_parse(&settings, body, len)) {
                uint32_t timestamp = 0;
                if ((p = kd_lookup(headers, "RTP-Info")) != NULL) sscanf(p, "%*[^=]=%lu", (unsigned long*)&timestamp);
				LOG_INFO("[%p]: received metadata (ts: %d)\n\tartist: %s\n\talbum:  %s\n\ttitle:  %s",
						 ctx, timestamp, metadata.artist ? metadata.artist : "", metadata.album ? metadata.album : "",
                         metadata.title ? metadata.title : "");
                success = ctx->cmd_cb(RAOP_INT_METADATA, metadata.artist, metadata.album, metadata.title, timestamp);
				free_metadata(&metadata);
			}
		} else if ((p = kd_lookup(headers, "Content-Type")) != NULL && strcasestr(p, "image/jpeg")) {
				uint32_t timestamp = 0;
				if ((p = kd_lookup(headers, "RTP-Info")) != NULL) sscanf(p, "%*[^=]=%lu", (unsigned long*)&timestamp);
				if (body) {
						LOG_INFO("[%p]: received JPEG image of %d bytes (ts:%d)", ctx, len, timestamp);
						ctx->cmd_cb(RAOP_INT_ARTWORK, body, len, timestamp);
				} else {
						LOG_INFO("[%p]: JPEG image discarded (too large: %d bytes)", ctx, len);
				}
		} else {
			char *dump = kd_dump(headers);
			LOG_INFO("Unhandled SET PARAMETER\n%s", dump);
			free(dump);
		}
	}

	if (strcmp(method, "OPTIONS")) {
    kd_add(resp, "Audio-Jack-Status", "connected; type=analog");
	}

	if (success) {
		buf = http_send(sock, "RTSP/1.0 200 OK", resp);
	} else {
		buf = http_send(sock, "RTSP/1.0 503 ERROR", NULL);
		closesocket(sock);
	}

	if (body) free(body);
	if (buf) free(buf);
	kd_free(resp);
	kd_free(headers);

	return true;
}

/*----------------------------------------------------------------------------*/
void cleanup_rtsp(raop_ctx_t *ctx, bool abort) {
	// first stop RTP process
	if (ctx->rtp) {
		rtp_end(ctx->rtp);
		ctx->rtp = NULL;
		if (abort) LOG_INFO("[%p]: RTP thread aborted", ctx);
		ctx->cmd_cb(RAOP_INT_TEARDOWN);
	}

	if (ctx->active_remote.running) {
		// need to make sure no search is on-going and reclaim task memory
		ctx->active_remote.running = false;
		xSemaphoreTake(ctx->active_remote.destroy_mutex, portMAX_DELAY);
		vTaskDelete(ctx->active_remote.thread);
		if (ctx->active_remote.xTaskBuffer) free(ctx->active_remote.xTaskBuffer);
		vSemaphoreDelete(ctx->active_remote.destroy_mutex);
		memset(&ctx->active_remote, 0, sizeof(ctx->active_remote));
		LOG_INFO("[%p]: Remote search thread aborted", ctx);
	}

	if (ctx->rtsp.aeskey) free(ctx->rtsp.aeskey);
	if (ctx->rtsp.aesiv) free(ctx->rtsp.aesiv);
	if (ctx->rtsp.fmtp) free(ctx->rtsp.fmtp);
	ctx->rtsp.aeskey = NULL;
	ctx->rtsp.aesiv = NULL;
	ctx->rtsp.fmtp = NULL;
}

/*----------------------------------------------------------------------------*/
static void search_remote(void *args) {
	raop_ctx_t *ctx = (raop_ctx_t*) args;
	bool found = false;

	LOG_INFO("starting remote search");

	while (ctx->active_remote.running && !found) {
		mdns_result_t *results = NULL;
		mdns_result_t *r;
		mdns_ip_addr_t *a;

		if (mdns_query_ptr("_dacp", "_tcp", 3000, 32,  &results)) {
			LOG_ERROR("mDNS active remote query Failed");
			continue;
		}

		for (r = results; r && !strcasestr(r->instance_name, ctx->active_remote.DACPid); r = r->next);
		if (r) {
			for (a = r->addr; a && a->addr.type != ESP_IPADDR_TYPE_V4; a = a->next);
			if (a) {
				found = true;
				ctx->active_remote.host.s_addr = a->addr.u_addr.ip4.addr;
				ctx->active_remote.port = r->port;
				LOG_INFO("found remote %s %s:%hu", r->instance_name, inet_ntoa(ctx->active_remote.host), ctx->active_remote.port);
			}
		}

		mdns_query_results_free(results);
	}

	// can't use xNotifyGive as it seems LWIP is using it as well
	xSemaphoreGive(ctx->active_remote.destroy_mutex);
	vTaskSuspend(NULL);
}

/*----------------------------------------------------------------------------*/
static char *rsa_apply(unsigned char *input, int inlen, int *outlen, int mode)
{
	const static char super_secret_key[] =
	"-----BEGIN RSA PRIVATE KEY-----\n"
	"MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
	"wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
	"wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
	"/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
	"UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
	"BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
	"LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
	"NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
	"lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
	"aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
	"a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
	"oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
	"oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
	"k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
	"AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
	"cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
	"54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
	"17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
	"1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
	"LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
	"2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
	"-----END RSA PRIVATE KEY-----";

	mbedtls_pk_context pkctx;
	mbedtls_rsa_context *trsa;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	const char *pers = "rsa_encrypt";
	size_t olen;
	int rc;

	// Initialize RNG (required for mbedtls v3)
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
	                      (const unsigned char *)pers, strlen(pers));

	mbedtls_pk_init(&pkctx);

	// Parse key with RNG (mbedtls v3 requires it)
	rc = mbedtls_pk_parse_key(&pkctx, (unsigned char *)super_secret_key,
	                          sizeof(super_secret_key), NULL, 0,
	                          mbedtls_ctr_drbg_random, &ctr_drbg);
	if (rc != 0) {
		LOG_ERROR("Error %d parsing private key", rc);
		mbedtls_ctr_drbg_free(&ctr_drbg);
		mbedtls_entropy_free(&entropy);
		mbedtls_pk_free(&pkctx);
		return NULL;
	}

	uint8_t *outbuf = NULL;
	trsa = mbedtls_pk_rsa(pkctx);

	switch (mode) {
	case RSA_MODE_AUTH:
		mbedtls_rsa_set_padding(trsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
		outbuf = malloc(mbedtls_rsa_get_len(trsa));

		// Use PKCS#1 v1.5 signature (private key operation)
		size_t sig_len = 0;
		rc = mbedtls_pk_sign(&pkctx, MBEDTLS_MD_NONE, input, inlen,
												outbuf, mbedtls_rsa_get_len(trsa), &sig_len,
												mbedtls_ctr_drbg_random, &ctr_drbg);

		if (rc != 0) {
			LOG_ERROR("RSA sign error %d", rc);
			free(outbuf);
			outbuf = NULL;
			*outlen = 0;
		} else {
			*outlen = sig_len;
		}
		break;

	case RSA_MODE_KEY:
		mbedtls_rsa_set_padding(trsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
		outbuf = malloc(mbedtls_rsa_get_len(trsa));
		rc = mbedtls_rsa_pkcs1_decrypt(trsa, mbedtls_ctr_drbg_random, &ctr_drbg,
		                               &olen, input, outbuf, mbedtls_rsa_get_len(trsa));
		if (rc != 0) {
			LOG_ERROR("RSA decrypt error %d", rc);
			free(outbuf);
			outbuf = NULL;
			*outlen = 0;
		} else {
			*outlen = olen;
		}
		break;
	}

	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	mbedtls_pk_free(&pkctx);

	return (char *)outbuf;
}

#define DECODE_ERROR 0xffffffff

const static char base64_chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*----------------------------------------------------------------------------*/
static int  base64_pad(char *src, char **padded)
{
	int n;

	n = strlen(src) + strlen(src) % 4;
	*padded = malloc(n + 1);
	memset(*padded, '=', n);
	memcpy(*padded, src, strlen(src));
	(*padded)[n] = '\0';

	return strlen(*padded);
}

/*----------------------------------------------------------------------------*/
static int pos(char c)
{
	const char *p;
	for (p = base64_chars; *p; p++)
	if (*p == c)
		return p - base64_chars;
	return -1;
}

/*----------------------------------------------------------------------------*/
static int base64_encode(const void *data, int size, char **str)
{
	char *s, *p;
	int i;
	int c;
	const unsigned char *q;

	p = s = (char *) malloc(size * 4 / 3 + 4);
	if (p == NULL) return -1;
	q = (const unsigned char *) data;
	i = 0;
	for (i = 0; i < size;) {
		c = q[i++];
		c *= 256;
		if (i < size) c += q[i];
		i++;
		c *= 256;
		if (i < size) c += q[i];
		i++;
		p[0] = base64_chars[(c & 0x00fc0000) >> 18];
		p[1] = base64_chars[(c & 0x0003f000) >> 12];
		p[2] = base64_chars[(c & 0x00000fc0) >> 6];
		p[3] = base64_chars[(c & 0x0000003f) >> 0];
		if (i > size) p[3] = '=';
		if (i > size + 1) p[2] = '=';
		p += 4;
	}
	*p = 0;
	*str = s;
	return strlen(s);
}

/*----------------------------------------------------------------------------*/
static unsigned int token_decode(const char *token)
{
	int i;
	unsigned int val = 0;
	int marker = 0;
	if (strlen(token) < 4)
	return DECODE_ERROR;
	for (i = 0; i < 4; i++) {
	val *= 64;
	if (token[i] == '=')
		marker++;
	else if (marker > 0)
		return DECODE_ERROR;
	else
		val += pos(token[i]);
	}
	if (marker > 2)
	return DECODE_ERROR;
	return (marker << 24) | val;
}

/*----------------------------------------------------------------------------*/
static int base64_decode(const char *str, void *data)
{
	const char *p;
	unsigned char *q;

	q = data;
	for (p = str; *p && (*p == '=' || strchr(base64_chars, *p)); p += 4) {
	unsigned int val = token_decode(p);
	unsigned int marker = (val >> 24) & 0xff;
	if (val == DECODE_ERROR)
		return -1;
	*q++ = (val >> 16) & 0xff;
	if (marker < 2)
		*q++ = (val >> 8) & 0xff;
	if (marker < 1)
		*q++ = val & 0xff;
	}
	return q - (unsigned char *) data;
}

/*----------------------------------------------------------------------------*/
static void on_dmap_string(void *ctx, const char *code, const char *name, const char *buf, size_t len) {
	struct metadata_s *metadata = (struct metadata_s *) ctx;

	if (!code || !buf) return;

	if (!strcasecmp(code, "asar")) metadata->artist = strndup(buf, len);
	else if (!strcasecmp(code, "asal")) metadata->album = strndup(buf, len);
	else if (!strcasecmp(code, "minm")) metadata->title = strndup(buf, len);
}