/* Standalone tests of the production scope and FFmpeg scheduling policy. */
#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lavf_mp4_policy.h"
#include "mov_bounded_interleave.h"

typedef struct Track {
    int enabled;
    int anchor;
    int unsupported;
    int64_t first_dts;
    int64_t step;
    int64_t base;
    int64_t stride;
    int64_t bytes;
    unsigned count;
    unsigned cursor;
} Track;

typedef struct Fixture {
    Track tracks[8];
    unsigned count;
} Fixture;

static int get_sample(void *opaque, unsigned index, MOVBoundedSample *sample)
{
    Fixture *f = opaque;
    assert(index < f->count);
    Track *t = &f->tracks[index];
    if (!t->enabled || t->cursor == t->count)
        return 0;
    if (t->unsupported)
        return -1;
    *sample = (MOVBoundedSample){
        .dts = t->first_dts + t->step * t->cursor,
        .pos = t->base + t->stride * t->cursor,
        .anchor = t->anchor,
    };
    return 1;
}

static int earliest(Fixture *f)
{
    int chosen = -1;
    MOVBoundedSample sample, best = {0};
    for (unsigned i = 0; i < f->count; i++) {
        if (get_sample(f, i, &sample) > 0 &&
            (chosen < 0 || sample.dts < best.dts)) {
            chosen = (int)i;
            best = sample;
        }
    }
    return chosen;
}

static Fixture separated(unsigned seconds)
{
    return (Fixture){
        .count = 2,
        .tracks = {
            { .enabled = 1, .anchor = 1, .step = 20000, .base = 10000,
              .stride = 4096, .bytes = 4096, .count = seconds * 50 },
            { .enabled = 1, .anchor = 1, .step = 20000,
              .base = INT64_C(128) * 1024 * 1024,
              .stride = 1024, .bytes = 1024, .count = seconds * 50 },
        },
    };
}

static void scope_tests(void)
{
    const char *positive[] = {"http://host/a", "https://host/a", "HTTPS://host/a"};
    const char *negative[] = {NULL, "", "h", "htt", "http", "http:", "http:/",
        "https", "https:", "https:/", "httpx://host", "file://a.mp4",
        "ftp://host/a.mp4", "memory://a", "edl://https://host/a"};
    for (unsigned i = 0; i < sizeof(positive) / sizeof(*positive); i++)
        assert(lavf_mp4_http_url(positive[i]));
    for (unsigned i = 0; i < sizeof(negative) / sizeof(*negative); i++)
        assert(!lavf_mp4_http_url(negative[i]));
    for (int mode = LAVF_MP4_LEGACY; mode <= LAVF_MP4_BOUNDED; mode++) {
        for (int bits = 0; bits < 64; bits++) {
            int mov = bits & 1, seek = bits & 2, duration = bits & 4;
            int network = bits & 8, http = bits & 16, explicit_opt = bits & 32;
            int expected = mode != LAVF_MP4_LEGACY && mov && seek && duration &&
                !explicit_opt && (mode == LAVF_MP4_BOUNDED || (network && http));
            assert(lavf_mp4_use_bounded(mode, mov, seek, duration, network,
                http ? "https://host/a" : "file:///a", explicit_opt) == !!expected);
        }
    }
    assert(lavf_mp4_preload_budget(8 * 1024 * 1024, 64 * 1024 * 1024) == 8 * 1024 * 1024);
    assert(lavf_mp4_preload_budget(8 * 1024 * 1024, 1024 * 1024) == 256 * 1024);
    assert(!lavf_mp4_preload_budget(8 * 1024 * 1024, 0));
    assert(!lavf_mp4_preload_budget(8 * 1024 * 1024, 256 * 1024 - 1));
    assert(!lavf_mp4_preload_budget(0, INT64_MAX));
    assert(!lavf_mp4_preload_budget(-1, INT64_MAX));
}

static unsigned run_fixture(Fixture *f, int64_t duration, int64_t budget,
                             int64_t *first_subtitle, int64_t *next_window_video)
{
    MOVBoundedInterleave state = {0};
    unsigned delivered = 0, transitions = 0;
    int previous = -1;
    ff_mov_bounded_reset(&state);
    while (1) {
        int choice = ff_mov_bounded_choose(&state, f->count, f, get_sample, duration, budget);
        if (choice < 0)
            choice = earliest(f);
        if (choice < 0)
            break;
        Track *track = &f->tracks[choice];
        MOVBoundedSample sample;
        assert(get_sample(f, choice, &sample) == 1);
        assert(choice >= 0 && (unsigned)choice < f->count);
        if (previous != choice)
            transitions++;
        previous = choice;
        if (!track->anchor && first_subtitle && *first_subtitle < 0)
            *first_subtitle = delivered;
        if (choice == 0 && sample.dts >= duration && next_window_video && *next_window_video < 0)
            *next_window_video = delivered;
        // Each selected packet is still the next packet of that stream; no
        // payload, timestamp, sample index or subtitle is skipped by the policy.
        if (state.valid && !state.disabled)
            assert(sample.dts < state.end);
        track->cursor++;
        ff_mov_bounded_record(&state, choice, track->bytes);
        delivered++;
        assert(delivered < 2000000);
    }
    unsigned expected = 0;
    for (unsigned i = 0; i < f->count; i++) {
        if (f->tracks[i].enabled) {
            expected += f->tracks[i].count;
            assert(f->tracks[i].cursor == f->tracks[i].count);
        }
    }
    assert(delivered == expected);
    return transitions;
}

static void scheduling_tests(void)
{
    Fixture f = separated(12);
    unsigned transitions = run_fixture(&f, 5000000, 8 * 1024 * 1024, NULL, NULL);
    assert(transitions == 6); // Three five-second windows, two physical runs each.
    printf("12 second separated tracks: %u runs for 1200 packets\n", transitions);

    f = separated(7200);
    f.tracks[1].base = INT64_C(16) * 1024 * 1024 * 1024;
    transitions = run_fixture(&f, 5000000, 8 * 1024 * 1024, NULL, NULL);
    assert(transitions <= 2881); // Never read a complete two-hour track first.
    printf("2 hour separated tracks: %u runs, all 720000 packets preserved\n", transitions);

    f = separated(12);
    f.tracks[0].bytes = 256 * 1024;
    f.tracks[0].stride = f.tracks[0].bytes;
    f.tracks[1].base = INT64_C(1024) * 1024 * 1024;
    MOVBoundedInterleave state = {0};
    for (int i = 0; i < 4; i++) {
        int chosen = ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 1024 * 1024);
        assert(chosen == 0);
        f.tracks[chosen].cursor++;
        ff_mov_bounded_record(&state, chosen, f.tracks[chosen].bytes);
    }
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 1024 * 1024) == 1);
    assert(state.capped && state.end == 80000); // Catch up, not just one audio packet.
    for (int i = 0; i < 4; i++) {
        assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 1024 * 1024) == 1);
        f.tracks[1].cursor++;
        ff_mov_bounded_record(&state, 1, f.tracks[1].bytes);
    }
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 1024 * 1024) == 0);

    f = separated(12);
    f.count = 3;
    f.tracks[2] = (Track){ .enabled = 1, .step = 4000000,
        .base = INT64_C(512) * 1024 * 1024, .stride = 64, .bytes = 64, .count = 3 };
    int64_t subtitle = -1, second_window = -1;
    run_fixture(&f, 5000000, 8 * 1024 * 1024, &subtitle, &second_window);
    assert(subtitle >= 0 && subtitle < second_window); // Sparse text cannot starve.

    f = separated(12);
    f.tracks[0].first_dts = f.tracks[1].first_dts = -1000000;
    run_fixture(&f, 5000000, 8 * 1024 * 1024, NULL, NULL); // Negative DTS / edit lists.

    f = separated(12);
    f.count = 3;
    f.tracks[2] = f.tracks[1];
    f.tracks[2].enabled = 0;
    f.tracks[2].base = 0; // Unselected audio cannot anchor or consume preload.
    run_fixture(&f, 5000000, 8 * 1024 * 1024, NULL, NULL);

    f = separated(12);
    f.count = 3;
    f.tracks[2] = f.tracks[1];
    f.tracks[2].enabled = 0;
    f.tracks[2].base = INT64_C(256) * 1024 * 1024;
    ff_mov_bounded_reset(&state);
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == 0);
    f.tracks[0].cursor = 200;
    f.tracks[1].enabled = 0;
    f.tracks[2].enabled = 1;
    f.tracks[2].cursor = 100;
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == 0);
    assert(state.start == 2000000 && state.end == 7000000);
    assert(state.active_mask == 5); // Switching audio resets the fixed window.

    // Frequent small subtitles can share the physical video region while
    // audio is far away. They must not reset the leading video byte budget.
    f = separated(12);
    f.tracks[0].bytes = 1024 * 1024;
    f.tracks[0].stride = 2 * 1024 * 1024;
    f.tracks[1].base = INT64_C(4) * 1024 * 1024 * 1024;
    f.count = 3;
    f.tracks[2] = (Track){ .enabled = 1, .first_dts = 60000, .step = 80000,
        .base = 10000 + 7 * 1024 * 1024, .stride = 8 * 1024 * 1024,
        .bytes = 64, .count = 150 };
    ff_mov_bounded_reset(&state);
    int64_t leading_bytes = 0;
    for (;;) {
        int chosen = ff_mov_bounded_choose(&state, f.count, &f, get_sample,
                                           5000000, 8 * 1024 * 1024);
        assert(chosen >= 0);
        if (chosen == 1)
            break;
        f.tracks[chosen].cursor++;
        leading_bytes += f.tracks[chosen].bytes;
        ff_mov_bounded_record(&state, chosen, f.tracks[chosen].bytes);
        if (leading_bytes > 8 * 1024 * 1024 + 4096)
            fprintf(stderr, "Subtitle reset exceeded preload budget before audio: %"PRId64" bytes\n",
                    leading_bytes);
        assert(leading_bytes <= 8 * 1024 * 1024 + 4096);
    }
    printf("Interspersed subtitles: audio reached after %"PRId64" bytes.\n", leading_bytes);
    for (unsigned i = 0; i < f.count; i++)
        f.tracks[i].cursor = 0;
    run_fixture(&f, 5000000, 8 * 1024 * 1024, NULL, NULL);

    // Short audio runs interspersed with much larger video runs must not
    // reset the window budget either, even if audio DTS still lags video.
    f = separated(12);
    f.tracks[0].bytes = 1024 * 1024;
    f.tracks[0].stride = 2 * 1024 * 1024;
    f.tracks[1].base = 10000 + 7 * 1024 * 1024;
    f.tracks[1].stride = 8 * 1024 * 1024;
    ff_mov_bounded_reset(&state);
    leading_bytes = 0;
    for (;;) {
        int chosen = ff_mov_bounded_choose(&state, f.count, &f, get_sample,
                                           5000000, 8 * 1024 * 1024);
        assert(chosen >= 0);
        // One packet can cross the budget before its size is known.
        assert(leading_bytes <= 9 * 1024 * 1024 + 4096);
        if (state.capped)
            break;
        f.tracks[chosen].cursor++;
        leading_bytes += f.tracks[chosen].bytes;
        ff_mov_bounded_record(&state, chosen, f.tracks[chosen].bytes);
    }
    printf("Interspersed audio: window capped after %"PRId64" bytes.\n", leading_bytes);
    for (unsigned i = 0; i < f.count; i++)
        f.tracks[i].cursor = 0;
    run_fixture(&f, 5000000, 8 * 1024 * 1024, NULL, NULL);

    for (int trial = 1; trial <= 128; trial++) {
        f = separated(1 + (unsigned)trial);
        f.tracks[0].step = 17000 + trial * 7;
        f.tracks[1].step = 21000 + trial * 11;
        f.tracks[0].count += (unsigned)trial;
        run_fixture(&f, 200000 + trial * 10000, 65536 + trial * 4096, NULL, NULL);
    }
}

static void fallback_and_seek_tests(void)
{
    Fixture f = separated(12);
    MOVBoundedInterleave state = {0};
    f.tracks[1].base = f.tracks[0].base + 4096;
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == -1);
    assert(!state.valid); // Normal interleaving is byte-for-byte legacy ordering.

    f = separated(12);
    f.tracks[1].unsupported = 1;
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == -1);
    assert(!state.valid);
    f.tracks[1].unsupported = 0;
    f.tracks[1].first_dts = INT64_MIN;
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == -1);

    f = separated(12);
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == 0);
    f.tracks[0].cursor = f.tracks[1].cursor = 400;
    ff_mov_bounded_reset(&state); // Production mov_read_seek does the same.
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == 0);
    assert(state.start == 8000000 && state.end == 13000000);
    f.tracks[0].cursor = f.tracks[1].cursor = 50;
    ff_mov_bounded_reset(&state);
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 8388608) == 0);
    assert(state.start == 1000000 && state.end == 6000000);

    f = separated(12);
    f.tracks[0].step = 0;
    ff_mov_bounded_reset(&state);
    for (int i = 0; i < 4; i++) {
        assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 16384) == 0);
        f.tracks[0].cursor++;
        ff_mov_bounded_record(&state, 0, 4096);
    }
    assert(ff_mov_bounded_choose(&state, f.count, &f, get_sample, 5000000, 16384) == -1);
    assert(state.disabled);
    ff_mov_bounded_reset(&state);
    assert(!state.disabled);
    assert(ff_mov_bounded_add(INT64_MAX - 3, 10) == INT64_MAX);
    assert(ff_mov_bounded_add(-100, 10) == -90);
}

int main(void)
{
    scope_tests();
    scheduling_tests();
    fallback_and_seek_tests();
    puts("MP4 policy tests passed: scope, bounds, long file, subtitles, seek, fallback, packet preservation.");
    return 0;
}
