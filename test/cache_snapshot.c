#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "demux/cache_snapshot.h"

static void check_missing(const int64_t *pts, int num_packets)
{
    struct mp_cache_snapshot_timestamp times[MP_CACHE_SNAPSHOT_MAX_PACKETS];
    for (int n = 0; n < num_packets; n++) {
        times[n] = (struct mp_cache_snapshot_timestamp) {
            .pts = pts[n], .dts = INT64_MIN,
        };
    }
    assert(mp_cache_snapshot_fix_dts(times, num_packets));
    for (int n = 0; n < num_packets; n++) {
        assert(times[n].pts == pts[n]);
        assert(times[n].dts <= times[n].pts);
        if (n)
            assert(times[n].dts > times[n - 1].dts);
    }
}

int main(void)
{
    // HEVC open-GOP order observed in MKV; native packets have no DTS/duration.
    const int64_t hevc[] = {133, 300, 66, 0, 33, 100, 266, 200, 166, 233};
    check_missing(hevc, sizeof(hevc) / sizeof(hevc[0]));
    // Unequal timestamp gaps and >16 reordered pictures must not assume CFR
    // or the recorder's old fixed reordering window.
    const int64_t vfr[] = {10000, 20000, 0, 1, 20, 100, 101, 1000, 9000};
    check_missing(vfr, sizeof(vfr) / sizeof(vfr[0]));
    int64_t large[MP_CACHE_SNAPSHOT_MAX_PACKETS];
    for (int n = 0; n < MP_CACHE_SNAPSHOT_MAX_PACKETS; n++)
        large[n] = (MP_CACHE_SNAPSHOT_MAX_PACKETS - n) / 3;
    check_missing(large, MP_CACHE_SNAPSHOT_MAX_PACKETS);
    const int64_t rounded[] = {1, 1, 0, 0, 0};
    check_missing(rounded, sizeof(rounded) / sizeof(rounded[0]));

    struct mp_cache_snapshot_timestamp complete[] = {{3, -2}, {0, -1}, {2, 0}};
    struct mp_cache_snapshot_timestamp saved[3];
    memcpy(saved, complete, sizeof(saved));
    assert(mp_cache_snapshot_fix_dts(complete, 3));
    assert(memcmp(saved, complete, sizeof(saved)) == 0);
    struct mp_cache_snapshot_timestamp mixed[] = {{3, 2}, {0, INT64_MIN}, {2, 1}};
    assert(mp_cache_snapshot_fix_dts(mixed, 3));
    assert(mixed[0].dts < mixed[1].dts && mixed[1].dts < mixed[2].dts);
    assert(mixed[0].pts == 3 && mixed[1].pts == 0 && mixed[2].pts == 2);

    struct mp_cache_snapshot_timestamp bad[] = {{1, INT64_MIN}, {INT64_MIN, 0}};
    assert(!mp_cache_snapshot_fix_dts(bad, 2));
    assert(bad[0].dts == INT64_MIN && bad[1].dts == 0);
    struct mp_cache_snapshot_timestamp overflow[] = {
        {INT64_MAX, INT64_MIN}, {INT64_MAX, INT64_MIN},
    };
    assert(!mp_cache_snapshot_fix_dts(overflow, 2));
    assert(overflow[0].dts == INT64_MIN && overflow[1].dts == INT64_MIN);
    struct mp_cache_snapshot_timestamp underflow[] = {
        {INT64_MAX, INT64_MIN}, {INT64_MIN + 1, INT64_MIN},
    };
    assert(!mp_cache_snapshot_fix_dts(underflow, 2));
    assert(underflow[0].dts == INT64_MIN && underflow[1].dts == INT64_MIN);
    assert(!mp_cache_snapshot_fix_dts(NULL, 0));
    assert(!mp_cache_snapshot_fix_dts(complete, MP_CACHE_SNAPSHOT_MAX_PACKETS + 1));
    puts("cache snapshot timestamp tests passed");
    return 0;
}
