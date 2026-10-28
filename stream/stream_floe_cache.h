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

struct floe_cache_stream_options {
    bool enabled;
    const char *directory;
    const char *key;
    int64_t max_bytes;
};

bool stream_floe_cache_wrap(struct stream *s,
                            const struct floe_cache_stream_options *options);

bool stream_floe_cache_get_state(struct stream *s, struct mpv_node *node);

