// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef MP_CACHE_SNAPSHOT_H
#define MP_CACHE_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#define MP_CACHE_SNAPSHOT_MAX_PACKETS 4096

struct mp_cache_snapshot_timestamp {
    int64_t pts;
    int64_t dts;
};

// Work only on a private, finite snapshot. INT64_MIN denotes a missing time.
// Preserve complete input DTS; otherwise derive a strictly increasing decode
// timeline without changing PTS or assuming a frame rate/reordering depth.
bool mp_cache_snapshot_fix_dts(struct mp_cache_snapshot_timestamp *times,
                               int num_packets);

#endif
