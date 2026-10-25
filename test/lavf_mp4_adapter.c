/* Compile the actual production adapters with small AVIO/option test doubles. */
#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lavf_mp4_policy.h"
#include "mov_bounded_interleave.h"

#define AV_TIME_BASE 1000000
#define AV_NOPTS_VALUE INT64_MIN
#define AVIO_SEEKABLE_NORMAL 1
#define AV_DISPOSITION_ATTACHED_PIC 1024
#define AVDISCARD_ALL 48
#define AVMEDIA_TYPE_VIDEO 0
#define AVMEDIA_TYPE_AUDIO 1
#define AVMEDIA_TYPE_SUBTITLE 3
#define AV_LOG_TRACE 0
#define AV_LOG_VERBOSE 0
#define AV_VERSION_INT(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#ifdef TEST_OLD_FFMPEG
#define LIBAVFORMAT_VERSION_INT AV_VERSION_INT(60, 16, 100)
#else
#define LIBAVFORMAT_VERSION_INT AV_VERSION_INT(63, 0, 100)
#endif
#define av_log(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)

typedef struct AVIOContext { int seekable; } AVIOContext;
typedef struct AVIndexEntry { int64_t timestamp, pos; } AVIndexEntry;
typedef struct FFStream { AVIndexEntry *index_entries; int nb_index_entries; } FFStream;
typedef struct AVCodecParameters { int codec_type; } AVCodecParameters;
typedef struct MOVStreamContext {
    AVIOContext *pb;
    void *iamf;
    int current_sample, time_scale;
} MOVStreamContext;
typedef struct AVStream {
    MOVStreamContext *priv_data;
    AVCodecParameters *codecpar;
    int discard, disposition, index;
    int64_t duration;
    FFStream internal;
} AVStream;
typedef struct MOVContext {
    MOVBoundedInterleave bounded_interleave;
    int interleave_strategy, interleaved_read;
    int64_t interleave_preload_duration, interleave_preload_max_bytes;
    struct { int nb_items; } frag_index;
    struct { int64_t moof_offset; } fragment;
    int64_t next_root_atom;
    int extension_available, fail_option, option_calls;
} MOVContext;
typedef struct AVFormatContext {
    MOVContext *priv_data;
    AVIOContext *pb;
    AVStream **streams;
    unsigned nb_streams;
#ifndef TEST_OLD_FFMPEG
    unsigned nb_stream_groups;
#endif
    int64_t duration;
} AVFormatContext;
typedef struct NativeInput { const char *url; } NativeInput;
typedef struct DemuxCache { int64_t max_bytes; } DemuxCache;

#ifndef TEST_OLD_FFMPEG
static FFStream *ffstream(AVStream *stream) { return &stream->internal; }
static int64_t av_rescale(int64_t value, int64_t numerator, int64_t denominator)
{
    assert(denominator == AV_TIME_BASE && numerator == AV_TIME_BASE);
    return value;
}
#endif

struct demux_lavf_opts {
    int mp4_read_strategy;
    double mp4_preload_secs;
    int64_t mp4_preload_bytes;
    char **avopts;
};
typedef struct lavf_priv {
    struct demux_lavf_opts *opts;
    AVFormatContext *avfc;
    NativeInput *stream;
    const char *format;
} lavf_priv_t;
struct demuxer {
    lavf_priv_t *priv;
    int is_network;
    DemuxCache *opts;
};

static int matches_avinputformat_name(lavf_priv_t *priv, const char *name)
{
    return strcmp(priv->format, name) == 0;
}
static const void *av_opt_find(MOVContext *ctx, const char *name,
                                const char *unit, int flags, int search)
{
    (void)unit; (void)flags; (void)search;
    assert(strcmp(name, "interleave_strategy") == 0);
    return ctx->extension_available ? ctx : NULL;
}
static int av_opt_set_int(MOVContext *ctx, const char *name, int64_t value, int flags)
{
    (void)flags;
    ctx->option_calls++;
    if (ctx->fail_option)
        return -1;
    if (strcmp(name, "interleave_preload_duration") == 0)
        ctx->interleave_preload_duration = value;
    else {
        assert(strcmp(name, "interleave_preload_max_bytes") == 0);
        ctx->interleave_preload_max_bytes = value;
    }
    return 0;
}
static int av_opt_set(MOVContext *ctx, const char *name, const char *value, int flags)
{
    (void)flags;
    assert(strcmp(name, "interleave_strategy") == 0 && strcmp(value, "bounded") == 0);
    ctx->option_calls++;
    ctx->interleave_strategy = 1;
    return 0;
}

/* Extracted unmodified from the patched FFmpeg and mpv source under test. */
#ifndef TEST_OLD_FFMPEG
#include "mov_adapter.inc"
#endif
#include "mpv_adapter.inc"

#ifndef TEST_OLD_FFMPEG
static void test_mp4_adapter(void)
{
    AVIOContext io = { .seekable = AVIO_SEEKABLE_NORMAL }, external = io;
    AVIndexEntry video[] = {{0, 10000}, {1200000, 14096}, {1240000, 18192}};
    AVIndexEntry audio[] = {{0, 134217728}, {20000, 134218752}, {40000, 134219776}};
    MOVStreamContext vs = { .pb = &io, .time_scale = AV_TIME_BASE };
    MOVStreamContext as = vs;
    AVCodecParameters vp = {AVMEDIA_TYPE_VIDEO}, ap = {AVMEDIA_TYPE_AUDIO};
    AVStream v = { .priv_data = &vs, .codecpar = &vp, .index = 0,
        .internal = {video, 3} };
    AVStream a = { .priv_data = &as, .codecpar = &ap, .index = 1,
        .internal = {audio, 3} };
    AVStream *streams[] = {&v, &a}, *chosen = NULL;
    MOVContext mov = { .interleave_strategy = 1, .interleaved_read = 1,
        .interleave_preload_duration = 5000000,
        .interleave_preload_max_bytes = 8388608 };
    AVFormatContext format = { .priv_data = &mov, .pb = &io,
        .streams = streams, .nb_streams = 2 };

    assert(mov_find_next_sample(&format, &chosen) == &video[0] && chosen == &v);
    vs.current_sample++;
    assert(mov_find_next_sample(&format, &chosen) == &video[1] && chosen == &v);
    assert(mov.bounded_interleave.valid);
    // Legacy sees the older audio timestamp; bounded order stays on video.
    assert(mov_find_next_sample_legacy(&format, &chosen) == &audio[0]);

    mov.frag_index.nb_items = 1;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    mov.frag_index.nb_items = 0;
    mov.fragment.moof_offset = 1;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    mov.fragment.moof_offset = 0;
    mov.next_root_atom = 1;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    mov.next_root_atom = 0;
    format.nb_stream_groups = 1;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    format.nb_stream_groups = 0;
    mov.interleave_strategy = 0;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    mov.interleave_strategy = 1;
    io.seekable = 0;
    assert(mov_find_next_sample(&format, &chosen) == &video[1]);
    io.seekable = 1;

    as.pb = &external;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    assert(!mov.bounded_interleave.valid);
    as.pb = &io;
    as.iamf = &mov;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    as.iamf = NULL;
    a.discard = AVDISCARD_ALL;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    a.discard = 0;
    a.disposition = AV_DISPOSITION_ATTACHED_PIC;
    assert(mov_find_next_sample(&format, &chosen) == &audio[0]);
    a.disposition = 0;
    audio[0].timestamp = AV_NOPTS_VALUE;
    AVIndexEntry *legacy = mov_find_next_sample_legacy(&format, &chosen);
    assert(mov_find_next_sample(&format, &chosen) == legacy);
    audio[0].timestamp = 0;
    puts("FFmpeg production adapter guards passed.");
}

static unsigned benchmark_mov_selector(int bounded)
{
    AVIOContext io = {.seekable = 1};
    AVIndexEntry video[600], audio[600];
    for (int i = 0; i < 600; i++) {
        video[i] = (AVIndexEntry){i * INT64_C(20000), 10000 + i * INT64_C(4096)};
        audio[i] = (AVIndexEntry){i * INT64_C(20000), 134217728 + i * INT64_C(1024)};
    }
    MOVStreamContext vs = {.pb = &io, .time_scale = AV_TIME_BASE}, as = vs;
    AVCodecParameters vp = {AVMEDIA_TYPE_VIDEO}, ap = {AVMEDIA_TYPE_AUDIO};
    AVStream v = {.priv_data = &vs, .codecpar = &vp, .index = 0, .internal = {video, 600}};
    AVStream a = {.priv_data = &as, .codecpar = &ap, .index = 1, .internal = {audio, 600}};
    AVStream *streams[] = {&v, &a}, *chosen = NULL;
    MOVContext mov = {.interleave_strategy = bounded, .interleaved_read = 1,
        .interleave_preload_duration = 5000000, .interleave_preload_max_bytes = 8388608};
    AVFormatContext format = {.priv_data = &mov, .pb = &io,
        .streams = streams, .nb_streams = 2};
    unsigned packets = 0, runs = 0;
    int previous = -1;
    for (;;) {
        AVIndexEntry *entry = mov_find_next_sample(&format, &chosen);
        if (!entry)
            break;
        assert(chosen && (chosen == &v || chosen == &a));
        assert(entry->timestamp == chosen->priv_data->current_sample * INT64_C(20000));
        if (previous != chosen->index)
            runs++;
        previous = chosen->index;
        chosen->priv_data->current_sample++;
        ff_mov_bounded_record(&mov.bounded_interleave, chosen->index,
                              chosen == &v ? 4096 : 1024);
        packets++;
        assert(packets <= 1200);
    }
    assert(packets == 1200 && vs.current_sample == 600 && as.current_sample == 600);
    return runs;
}
#endif

static void test_mpv_adapter(void)
{
    AVIOContext io = {.seekable = 1};
    MOVContext mov = {.extension_available = 1};
    AVFormatContext avfc = {.priv_data = &mov, .pb = &io, .duration = 10000000};
    struct demux_lavf_opts opts = {.mp4_read_strategy = LAVF_MP4_AUTO,
        .mp4_preload_secs = 5, .mp4_preload_bytes = 8388608};
    lavf_priv_t priv = {.avfc = &avfc, .opts = &opts, .format = "mov"};
    struct demuxer demuxer = {.priv = &priv, .is_network = 1};
    NativeInput stream = {"https://host/movie"};
    DemuxCache cache = {64 * 1024 * 1024};
    priv.stream = &stream;
    demuxer.opts = &cache;
    setup_mp4_bounded_interleave(&demuxer);
    assert(mov.interleave_strategy == 1 && mov.option_calls == 3);
    assert(mov.interleave_preload_duration == 5000000);
    assert(mov.interleave_preload_max_bytes == 8388608);
    mov.interleave_strategy = 0;
    mov.option_calls = 0;

    const char *excluded[] = {"matroska", "hls", "mpegts", "bluray", "avi"};
    for (unsigned i = 0; i < sizeof(excluded) / sizeof(*excluded); i++) {
        priv.format = excluded[i];
        setup_mp4_bounded_interleave(&demuxer);
        assert(!mov.option_calls);
    }
    priv.format = "mov";
#ifndef TEST_OLD_FFMPEG
    avfc.nb_stream_groups = 1;
    setup_mp4_bounded_interleave(&demuxer);
    assert(!mov.option_calls);
    avfc.nb_stream_groups = 0;
#endif
    mov.extension_available = 0;
    setup_mp4_bounded_interleave(&demuxer);
    assert(!mov.option_calls && !mov.interleave_strategy);
    mov.extension_available = 1;
    char *raw[] = {"interleaved_read", "0", NULL};
    opts.avopts = raw;
    setup_mp4_bounded_interleave(&demuxer);
    assert(!mov.option_calls);
    opts.avopts = NULL;
    cache.max_bytes = 1024 * 1024;
    setup_mp4_bounded_interleave(&demuxer);
    assert(mov.interleave_preload_max_bytes == 256 * 1024);
    mov.interleave_strategy = 0;
    mov.option_calls = 0;
    mov.fail_option = 1;
    setup_mp4_bounded_interleave(&demuxer);
    assert(mov.option_calls == 1 && !mov.interleave_strategy);
    puts("mpv production adapter opt-in, cache clamp and old-FFmpeg fallback passed.");
}

int main(void)
{
#ifndef TEST_OLD_FFMPEG
    test_mp4_adapter();
#endif
    test_mpv_adapter();
#ifndef TEST_OLD_FFMPEG
    unsigned legacy = benchmark_mov_selector(0), bounded = benchmark_mov_selector(1);
    assert(legacy > 1000 && bounded == 6);
    printf("Production MOV selector: %u -> %u physical runs; all 1200 packet timestamps preserved.\n",
           legacy, bounded);
#endif
    return 0;
}
