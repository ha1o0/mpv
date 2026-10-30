// SPDX-License-Identifier: LGPL-2.1-or-later

#include <limits.h>
#include <stdlib.h>

#include "cache_snapshot.h"

static int compare_time(const void *pa, const void *pb)
{
    int64_t a = *(const int64_t *)pa;
    int64_t b = *(const int64_t *)pb;
    return (a > b) - (a < b);
}

bool mp_cache_snapshot_fix_dts(struct mp_cache_snapshot_timestamp *times,
                               int num_packets)
{
    if (!times || num_packets <= 0 || num_packets > MP_CACHE_SNAPSHOT_MAX_PACKETS)
        return false;

    bool missing = false;
    for (int n = 0; n < num_packets; n++)
        missing |= times[n].dts == INT64_MIN;
    if (!missing)
        return true;

    int64_t *sorted = malloc((size_t)num_packets * sizeof(*sorted));
    if (!sorted)
        return false;
    bool ok = false;
    for (int n = 0; n < num_packets; n++) {
        if (times[n].pts == INT64_MIN)
            goto done;
        sorted[n] = times[n].pts;
    }
    qsort(sorted, num_packets, sizeof(*sorted), compare_time);

    int64_t shift = 0;
    for (int n = 0; n < num_packets; n++) {
        // Duplicate/rounded PTS still need distinct DTS in the NUT muxer.
        if (n && sorted[n] <= sorted[n - 1]) {
            if (sorted[n - 1] == INT64_MAX)
                goto done;
            sorted[n] = sorted[n - 1] + 1;
        }
        if (sorted[n] > times[n].pts) {
            if (times[n].pts < 0 && sorted[n] > INT64_MAX + times[n].pts)
                goto done;
            int64_t delta = sorted[n] - times[n].pts;
            if (delta > shift)
                shift = delta;
        }
    }
    for (int n = 0; n < num_packets; n++) {
        if (sorted[n] <= INT64_MIN + shift)
            goto done;
    }
    // The smallest common shift keeps DTS <= the corresponding original PTS.
    // Rebuild all DTS together when any are absent, so mixed input cannot join
    // two incompatible decode timelines. Packet order and display times stay
    // intact; these values are only for muxing this single-video-track clip.
    for (int n = 0; n < num_packets; n++)
        times[n].dts = sorted[n] - shift;
    ok = true;
done:
    free(sorted);
    return ok;
}
