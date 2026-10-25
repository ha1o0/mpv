/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */

#ifndef MP_LAVF_MP4_POLICY_H
#define MP_LAVF_MP4_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum lavf_mp4_read_strategy {
    LAVF_MP4_LEGACY,
    LAVF_MP4_AUTO,
    LAVF_MP4_BOUNDED,
};

static inline bool lavf_mp4_http_url(const char *url)
{
    if (!url)
        return false;
    const char *prefix = "http";
    for (; *prefix; prefix++, url++) {
        char c = *url;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != *prefix)
            return false;
    }
    if (*url == 's' || *url == 'S')
        url++;
    return url[0] == ':' && url[1] == '/' && url[2] == '/';
}

static inline bool lavf_mp4_use_bounded(int strategy, bool is_mov,
                                      bool seekable, bool finite_duration,
                                      bool is_network, const char *url,
                                      bool explicit_interleave)
{
    if (strategy == LAVF_MP4_LEGACY || !is_mov || !seekable ||
        !finite_duration || explicit_interleave)
        return false;
    return strategy == LAVF_MP4_BOUNDED ||
           (strategy == LAVF_MP4_AUTO && is_network && lavf_mp4_http_url(url));
}

static inline int64_t lavf_mp4_preload_budget(int64_t requested,
                                            int64_t forward_cache)
{
    // Leave space for the lagging track and packet overhead. Very small or
    // disabled demuxer caches should retain the existing packet order.
    const int64_t minimum = 64 * 1024;
    if (requested < minimum || forward_cache < 4 * minimum)
        return 0;
    int64_t limit = forward_cache / 4;
    return requested < limit ? requested : limit;
}

#endif
