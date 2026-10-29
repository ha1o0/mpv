/*
 * Floe persistent byte cache for native mpv streams.
 *
 * This layer deliberately sits below the demuxer. It stores the bytes read
 * from an HTTP stream and is independent of whether the lower stream points
 * at a remote server or at Floe's loopback gateway.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mpv_node;
struct stream;
struct stream_open_args;
struct floe_cache_curl_session;

#define FLOE_CACHE_CHUNK_SIZE (4 * 1024 * 1024)

struct floe_cache_http_response {
    bool cacheable;
    bool changed;
    int status;
    int64_t start;
    int64_t end;
    int64_t length;
    char validator[2049];
    char validator_header[20];
    char content_type[513];
};

struct floe_cache_stream_options {
    bool enabled;
    const char *directory;
    const char *key;
    const char *url;
    const char *source;
    int64_t max_bytes;
};

int stream_floe_cache_version(void);
int stream_floe_cache_open(struct stream *s, struct stream_open_args *args,
                          const struct floe_cache_stream_options *options);

void stream_floe_cache_parse_http(struct floe_cache_http_response *out,
    int status, int64_t content_length, const char *content_range,
    const char *accept_ranges, const char *etag, const char *modified,
    const char *content_type, const char *encoding,
    const char *cache_control, const char *vary);
bool stream_floe_cache_validate_http(const struct floe_cache_http_response *out,
    const struct floe_cache_http_response *expected, int64_t start, int64_t end);

// Reuses the curl transport's auth, proxy and retry policy. Every response,
// including reconnects, is checked before its body can enter the byte cache.
int stream_curl_floe_cache_open(struct stream *parent, int64_t start, int64_t end,
    struct floe_cache_curl_session *session,
    const struct floe_cache_http_response *expected, struct stream **stream,
    struct floe_cache_http_response *response);
struct floe_cache_curl_session *stream_curl_floe_cache_snapshot(void *ctx, struct stream *s);
bool stream_curl_floe_cache_allowed(struct floe_cache_curl_session *session);

bool stream_floe_cache_get_state(struct stream *s, struct mpv_node *node);

