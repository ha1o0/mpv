// Exercise the production cache with deterministic HTTP response/stream inputs.
// Full curl/libmpv HTTP coverage is in Floe's playback_disk_cache_native_test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/hash.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "stream/stream.h"
#include "stream/stream_floe_cache.h"
#include "stream/floe_cache_file.h"
#ifndef _WIN32
#include <sys/file.h>
#endif

struct input {
    int length, opens, status;
    char fill;
    const char *etag, *control, *type;
    bool offline, ignore_range, cache_allowed;
};
struct lower {
    struct floe_cache_http_response response;
    int64_t end;
    char fill;
};
static struct input input;
static char *base;
static const char *source = "12345678-1234-1234-1234-123456789abc";
static const char *url = "https://origin/movie.mp4";

static FILE *cache_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide = floe_cache_windows_path(NULL, path);
    if (!wide) return NULL;
    wchar_t *wide_mode = mp_from_utf8(NULL, mode);
    FILE *file = _wfopen(wide, wide_mode);
    talloc_free(wide_mode); talloc_free(wide);
    return file;
#else
    return fopen(path, mode);
#endif
}

struct floe_cache_curl_session *stream_curl_floe_cache_snapshot(void *ctx, struct stream *s)
{
    return NULL;
}
bool stream_curl_floe_cache_allowed(struct floe_cache_curl_session *session)
{
    return input.cache_allowed;
}

void free_stream(struct stream *s)
{
    if (s->close) s->close(s);
    talloc_free(s);
}
int64_t stream_get_size(struct stream *s) { return s->get_size(s); }
int stream_control(struct stream *s, int command, void *out)
{
    return s->control ? s->control(s, command, out) : STREAM_UNSUPPORTED;
}
static int64_t lower_size(struct stream *s)
{
    return ((struct lower *)s->priv)->response.length;
}
static int lower_control(struct stream *s, int command, void *out)
{
    if (command != STREAM_CTRL_GET_FLOE_HTTP_RESPONSE) return STREAM_UNSUPPORTED;
    *(struct floe_cache_http_response *)out = ((struct lower *)s->priv)->response;
    return STREAM_OK;
}
int stream_read_partial(struct stream *s, void *buffer, int maximum)
{
    struct lower *lower = s->priv;
    int count = MPMIN((int64_t)maximum, lower->end - s->pos);
    if (count <= 0) return 0;
    memset(buffer, lower->fill, count);
    s->pos += count;
    return count;
}
bool stream_seek(struct stream *s, int64_t position)
{
    s->pos = position;
    return 1;
}
int stream_curl_floe_cache_open(struct stream *parent, int64_t start, int64_t end,
    struct floe_cache_curl_session *session, const struct floe_cache_http_response *expected,
    struct stream **out, struct floe_cache_http_response *response)
{
    input.opens++;
    *out = NULL;
    *response = (struct floe_cache_http_response){0};
    int status = input.offline ? 503 : input.status;
    if (input.ignore_range) status = 200;
    int64_t actual_start = status == 200 ? 0 : start;
    int64_t actual_end = status == 200 || end <= 0 ? input.length : MPMIN(end, input.length);
    char range[128];
    snprintf(range, sizeof(range), "bytes %lld-%lld/%d",
        (long long)actual_start, (long long)actual_end - 1, input.length);
    stream_floe_cache_parse_http(response, status, actual_end - actual_start,
        range, "bytes", input.etag, NULL, input.type, NULL, input.control, NULL);
    if (expected && !stream_floe_cache_validate_http(response, expected, start, end)) {
        response->changed = status == 200 || status == 206 || status == 416;
        return STREAM_ERROR;
    }
    if (status >= 400) return STREAM_ERROR;
    struct stream *s = talloc_zero(NULL, struct stream);
    struct lower *lower = talloc_zero(s, struct lower);
    *lower = (struct lower){.response = *response, .end = actual_end, .fill = input.fill};
    s->priv = lower; s->pos = start; s->seekable = true;
    s->get_size = lower_size; s->control = lower_control;
    *out = s;
    return STREAM_OK;
}

static void reset_input(void)
{
    input = (struct input){.length = 128 * 1024, .fill = 'A',
        .etag = "\"v1\"", .type = "video/mp4", .status = 206, .cache_allowed = true};
}
static char *key_for(const char *name)
{
    void *ctx = talloc_new(NULL);
    bstr digest = mp_hash_to_bstr(ctx, (const uint8_t *)name, strlen(name), "SHA256");
    char *key = bstrto0(NULL, digest);
    for (int i = 0; i < 64; i++) key[i] = tolower((unsigned char)key[i]);
    talloc_free(ctx);
    return key;
}
static struct stream *open_cache(const char *root, const char *key, int64_t limit)
{
    struct stream *s = talloc_zero(NULL, struct stream);
    s->url = talloc_strdup(s, url);
    struct stream_open_args args = {.url = url};
    struct floe_cache_stream_options options = {
        .enabled = true, .directory = root, .key = key, .url = url,
        .source = source, .max_bytes = limit,
    };
    assert(stream_floe_cache_open(s, &args, &options) == STREAM_OK);
    return s;
}
static int read_cache(struct stream *s, int64_t offset, int count, char expected)
{
    if (s->pos != offset) {
        assert(s->seek(s, offset) == 1);
        s->pos = offset;
    }
    uint8_t *bytes = malloc(count);
    int got = s->fill_buffer(s, bytes, count);
    for (int i = 0; i < got; i++) assert(bytes[i] == (uint8_t)expected);
    s->pos += got;
    free(bytes);
    return got;
}
static char *media_directory(const char *root, const char *key)
{
    char *scope = mp_path_join(NULL, root, source);
    char *directory = mp_path_join(NULL, scope, key);
    talloc_free(scope);
    return directory;
}
static char *chunk_file(const char *root, const char *key)
{
    char *directory = media_directory(root, key), *path = NULL;
    DIR *dir = floe_cache_opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n > 6 && !strcmp(entry->d_name + n - 6, ".chunk")) {
            path = mp_path_join(NULL, directory, entry->d_name);
            break;
        }
    }
    closedir(dir); talloc_free(directory);
    return path;
}
static int64_t data_size(const char *directory)
{
    DIR *dir = floe_cache_opendir(directory);
    if (!dir) return 0;
    int64_t size = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char *path = mp_path_join(NULL, directory, entry->d_name);
        struct stat st;
        assert(floe_cache_lstat(path, &st) == 0);
        if (S_ISDIR(st.st_mode)) size += data_size(path);
        else if (S_ISREG(st.st_mode)) size += st.st_size;
        talloc_free(path);
    }
    closedir(dir);
    return size;
}
static void clear_media(const char *root, const char *key)
{
    char *directory = media_directory(root, key);
    DIR *dir = floe_cache_opendir(directory);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char *path = mp_path_join(NULL, directory, entry->d_name);
            assert(floe_cache_unlink(path) == 0); talloc_free(path);
        }
        closedir(dir);
    }
    char *epoch = mp_path_join(NULL, root, ".epoch");
    FILE *file = cache_fopen(epoch, "wb"); assert(file);
    for (int i = 0; i < 64; i++) fputc('0', file);
    assert(fclose(file) == 0);
    talloc_free(epoch); talloc_free(directory);
}
static void seed(const char *root, const char *key, int count)
{
    struct stream *s = open_cache(root, key, 1024 * 1024);
    assert(read_cache(s, 0, count, input.fill) == count);
    free_stream(s);
}

static void test_bypass(void)
{
    char *root = mp_path_join(NULL, base, "bypass"), *key = key_for("movie");
    for (int variant = 0; variant < 15; variant++) {
        reset_input();
        struct stream *s = talloc_zero(NULL, struct stream);
        s->url = talloc_strdup(s, url);
        s->priv = &input;
        s->pos = 42;
        struct stream_open_args args = {.url = s->url};
        struct floe_cache_stream_options options = {
            .enabled = true, .directory = root, .key = key, .url = s->url,
            .source = source, .max_bytes = 1024 * 1024,
        };
        if (variant == 1) options.enabled = false;
        if (variant == 2) options.directory = NULL;
        if (variant == 3) options.directory = "";
        if (variant == 4) options.max_bytes = 0;
        if (variant == 5) options.key = "invalid";
        if (variant == 6) options.url = NULL;
        if (variant == 7) options.url = "https://origin/other.mp4";
        if (variant == 8) args.no_floe_cache = true;
        if (variant == 9) args.special_arg = s;
        if (variant == 10) args.flags |= STREAM_LOCAL_FS_ONLY;
        if (variant == 11) s->mode = STREAM_WRITE;
        if (variant == 12) {
            talloc_free(s->url);
            args.url = options.url = s->url = talloc_strdup(s, "file:///movie.mp4");
        }
        if (variant == 13) options.source = "../foreign";
        if (variant == 14) input.cache_allowed = false;
        uint8_t original[sizeof(*s)];
        memcpy(original, s, sizeof(original));
        assert(stream_floe_cache_open(s, &args, variant == 0 ? NULL : &options) == STREAM_NO_MATCH);
        assert(!memcmp(original, s, sizeof(original)));
        assert(input.opens == 0);
        assert(floe_cache_path_missing(root));
        free_stream(s);
    }
    talloc_free(key); talloc_free(root);
    puts("disabled, excluded and unavailable transports leave the stream and disk untouched");
}

static void test_offline_and_related(void)
{
    reset_input();
    char *root = mp_path_join(NULL, base, "offline"), *key = key_for("movie");
    seed(root, key, input.length);
    input.offline = true; input.opens = 0;
    struct stream *s = open_cache(root, key, 1024 * 1024);
    assert(stream_get_size(s) == input.length);
    assert(read_cache(s, 64000, 8192, 'A') == 8192);
    assert(input.opens == 0);
    free_stream(s);
    s = talloc_zero(NULL, struct stream);
    s->url = talloc_strdup(s, "https://origin/subtitle.srt");
    struct stream_open_args args = {.url = s->url};
    struct floe_cache_stream_options opts = {.enabled = true, .directory = root,
        .key = key, .url = url, .source = source, .max_bytes = 1024 * 1024};
    assert(stream_floe_cache_open(s, &args, &opts) == STREAM_NO_MATCH);
    assert(!s->floe_cache);
    free_stream(s); talloc_free(key); talloc_free(root);
    puts("offline reopen/seek and related HTTP stream isolation passed");
}

static MP_THREAD_VOID release_guard(void *arg)
{
    mp_sleep_ns(10 * 1000 * 1000);
    assert(close(*(int *)arg) == 0);
    MP_THREAD_RETURN();
}

static void test_lock_contention(void)
{
    reset_input();
    char *root = mp_path_join(NULL, base, "contention"), *key = key_for("movie");
    struct stream *s = open_cache(root, key, 1024 * 1024);
    char *path = mp_path_join(NULL, root, ".lock-v2");
    int fd = floe_cache_open(path, O_RDWR | O_BINARY, 0); assert(fd >= 0);
#ifdef _WIN32
    OVERLAPPED position = {0};
    assert(LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK |
        LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &position));
#else
    assert(flock(fd, LOCK_EX | LOCK_NB) == 0);
#endif
    mp_thread thread;
    assert(mp_thread_create(&thread, release_guard, &fd) == 0);
    assert(read_cache(s, 0, input.length, 'A') == input.length);
    assert(mp_thread_join(thread) == 0);
    free_stream(s);
    input.offline = true; input.opens = 0;
    s = open_cache(root, key, 1024 * 1024);
    assert(read_cache(s, 0, input.length, 'A') == input.length);
    assert(input.opens == 0); free_stream(s);
    talloc_free(path); talloc_free(root); talloc_free(key);
    puts("short management lock contention preserves publication and offline hits");
}

static void test_changes(void)
{
    for (int change = 0; change < 3; change++) {
        reset_input();
        char *name = talloc_asprintf(NULL, "change-%d", change);
        char *root = mp_path_join(NULL, base, name), *key = key_for("movie");
        seed(root, key, 32768);
        if (change == 0) input.etag = "\"v2\"";
        if (change == 1) input.length += 32768;
        if (change == 2) input.ignore_range = true;
        input.fill = 'B'; input.opens = 0;
        struct stream *s = open_cache(root, key, 1024 * 1024);
        assert(read_cache(s, 0, 32768, 'A') == 32768);
        assert(read_cache(s, 32768, 8192, 'B') == 0);
        assert(data_size(root) == 0);
        free_stream(s);
        talloc_free(name); talloc_free(root); talloc_free(key);
    }
    reset_input();
    char *root = mp_path_join(NULL, base, "transient"), *key = key_for("movie");
    seed(root, key, 32768);
    int64_t before = data_size(root);
    input.offline = true;
    struct stream *s = open_cache(root, key, 1024 * 1024);
    assert(read_cache(s, 32768, 8192, 'A') == 0);
    free_stream(s);
    assert(data_size(root) == before);
    talloc_free(root); talloc_free(key);
    puts("ETag/length/Range changes never splice bytes; transient failures retain cache");
}

static void test_corruption(void)
{
    for (int truncated = 0; truncated < 2; truncated++) {
        reset_input();
        char *name = talloc_asprintf(NULL, "corrupt-%d", truncated);
        char *root = mp_path_join(NULL, base, name), *key = key_for("movie");
        seed(root, key, input.length);
        char *chunk = chunk_file(root, key); assert(chunk);
        FILE *file = cache_fopen(chunk, truncated ? "wb" : "r+b"); assert(file);
        fputc('X', file); assert(fclose(file) == 0);
        input.opens = 0;
        struct stream *s = open_cache(root, key, 1024 * 1024);
        assert(read_cache(s, 0, input.length, 'A') == input.length);
        assert(input.opens == 1);
        free_stream(s);
        input.offline = true; input.opens = 0;
        s = open_cache(root, key, 1024 * 1024);
        assert(read_cache(s, 0, input.length, 'A') == input.length);
        assert(input.opens == 0);
        free_stream(s);
        talloc_free(chunk); talloc_free(name); talloc_free(root); talloc_free(key);
    }
    puts("damaged and truncated chunks are repaired and reopen offline");
}

static void test_quota_and_clear(void)
{
    reset_input(); input.length = 96 * 1024;
    char *root = mp_path_join(NULL, base, "quota"), *a = key_for("a"), *b = key_for("b");
    struct stream *s = open_cache(root, a, 128 * 1024);
    assert(read_cache(s, 0, input.length, 'A') == input.length); free_stream(s);
    s = open_cache(root, b, 128 * 1024);
    assert(read_cache(s, 0, input.length, 'A') == input.length); free_stream(s);
    assert(data_size(root) <= 128 * 1024);
    talloc_free(root); talloc_free(a); talloc_free(b);

    reset_input(); root = mp_path_join(NULL, base, "clear"); a = key_for("movie");
    struct stream *old = open_cache(root, a, 1024 * 1024);
    assert(read_cache(old, 0, 16384, 'A') == 16384);
    clear_media(root, a);
    seed(root, a, 16384);
    int64_t size = data_size(root);
    assert(read_cache(old, 16384, 16384, 'A') == 16384);
    input.etag = "\"v2\"";
    assert(read_cache(old, 65536, 8192, 'A') == 0);
    free_stream(old);
    assert(data_size(root) == size);
    input.offline = true; input.opens = 0;
    s = open_cache(root, a, 1024 * 1024);
    assert(read_cache(s, 0, 16384, 'A') == 16384);
    assert(input.opens == 0); free_stream(s);
    talloc_free(root); talloc_free(a);
    puts("root quota and clear revocation protect newer same-version cache");
}

static void test_uncacheable(void)
{
    for (int variant = 0; variant < 3; variant++) {
        reset_input();
        if (variant == 0) input.etag = NULL;
        if (variant == 1) input.control = "no-store";
        if (variant == 2) input.type = "application/vnd.apple.mpegurl";
        char *name = talloc_asprintf(NULL, "uncacheable-%d", variant);
        char *root = mp_path_join(NULL, base, name), *key = key_for("movie");
        struct stream *s = open_cache(root, key, 1024 * 1024);
        assert(read_cache(s, 0, input.length, 'A') == input.length);
        int opens = input.opens;
        assert(read_cache(s, input.length, 8192, 'A') == 0);
        assert(input.opens == opens);
        assert(read_cache(s, 16384, 8192, 'A') == 8192);
        assert(input.opens == opens);
        free_stream(s); assert(data_size(root) == 0);
        talloc_free(name); talloc_free(root); talloc_free(key);
    }
    puts("unversioned, no-store and manifest responses remain uncached");
}

static void test_initial_200(void)
{
    reset_input(); input.status = 200;
    char *root = mp_path_join(NULL, base, "initial-200"), *key = key_for("movie");
    struct stream *s = open_cache(root, key, 1024 * 1024);
    assert(stream_get_size(s) == input.length);
    assert(read_cache(s, 0, input.length, 'A') == input.length);
    int opens = input.opens;
    assert(read_cache(s, input.length, 8192, 'A') == 0);
    assert(input.opens == opens);
    free_stream(s);
    input.offline = true; input.opens = 0;
    s = open_cache(root, key, 1024 * 1024);
    assert(read_cache(s, 0, input.length, 'A') == input.length);
    assert(input.opens == 0); free_stream(s);
    talloc_free(root); talloc_free(key);
    puts("200 initial response records exact length and does not request past EOF");
}

static void test_file_paths(void)
{
    reset_input();
    char *root = mp_path_join(NULL, base, "\xE6\x96\x87\xE4\xBB\xB6\xE7\xBC\x93\xE5\xAD\x98");
    assert(floe_cache_mkdir(root, 0700) == 0);
    for (int i = 0; i < 4; i++) {
        char *next = mp_path_join(NULL, root,
            "long-cache-directory-0123456789012345678901234567890123456789");
        assert(floe_cache_mkdir(next, 0700) == 0);
        talloc_free(root); root = next;
    }
    char *key = key_for("unicode-path");
    seed(root, key, input.length);
    char *chunk = chunk_file(root, key); assert(chunk);
    assert(strlen(chunk) > 260);
    struct stat st;
    assert(floe_cache_lstat(chunk, &st) == 0 && S_ISREG(st.st_mode));
    assert(st.st_size == input.length);
    assert(floe_cache_touch(chunk) == 0);
    assert(!floe_cache_path_missing(chunk));
    char *missing = mp_path_join(NULL, root, "missing");
    assert(floe_cache_path_missing(missing));
    char *renamed = talloc_asprintf(NULL, "%s.renamed", chunk);
    assert(floe_cache_rename(chunk, renamed) == 0);
    assert(floe_cache_path_missing(chunk));
    assert(floe_cache_lstat(renamed, &st) == 0 && st.st_size == input.length);
    assert(floe_cache_rename(renamed, chunk) == 0);
    char *link = mp_path_join(NULL, root, "link.chunk");
    bool created = false;
#ifdef _WIN32
    wchar_t *wide_link = floe_cache_windows_path(NULL, link);
    wchar_t *wide_target = floe_cache_windows_path(NULL, chunk);
    created = CreateSymbolicLinkW(wide_link, wide_target, 0x2) != 0;
    talloc_free(wide_link); talloc_free(wide_target);
#else
    created = symlink(chunk, link) == 0;
#endif
    if (created) {
        assert(floe_cache_lstat(link, &st) == 0);
        assert(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode));
        assert(!floe_cache_path_missing(link));
        assert(floe_cache_unlink(link) == 0);
    }
    input.offline = true; input.opens = 0;
    struct stream *s = open_cache(root, key, 1024 * 1024);
    assert(read_cache(s, 0, input.length, 'A') == input.length);
    assert(input.opens == 0);
    struct mpv_node state;
    assert(stream_floe_cache_get_state(s, &state));
    struct mpv_node *cached = node_map_get(&state, "cached-bytes");
    struct mpv_node *ranges = node_map_get(&state, "ranges");
    assert(cached && cached->format == MPV_FORMAT_INT64 && cached->u.int64 == input.length);
    assert(ranges && ranges->format == MPV_FORMAT_NODE_ARRAY && ranges->u.list->num > 0);
    talloc_free(state.u.list);
    free_stream(s);
#ifdef _WIN32
    char *extended = floe_cache_windows_utf8_path(root);
    assert(extended);
    s = open_cache(extended, key, 1024 * 1024);
    assert(read_cache(s, 0, input.length, 'A') == input.length);
    assert(input.opens == 0); free_stream(s); talloc_free(extended);

    const char *paths[][2] = {
        {"C:/cache/../video/file.chunk", "\\\\?\\C:\\video\\file.chunk"},
        {"//cache-server/share/video/file.chunk", "\\\\?\\UNC\\cache-server\\share\\video\\file.chunk"},
        {"\\\\?\\C:\\video/file.chunk", "\\\\?\\C:\\video\\file.chunk"},
        {"\\\\?\\UNC\\cache-server\\share/video/file.chunk", "\\\\?\\UNC\\cache-server\\share\\video\\file.chunk"},
    };
    for (int i = 0; i < MP_ARRAY_SIZE(paths); i++) {
        char *native = floe_cache_windows_utf8_path(paths[i][0]);
        assert(native && !strcmp(native, paths[i][1])); talloc_free(native);
    }
#endif
    clear_media(root, key);
    assert(!chunk_file(root, key));
    assert(data_size(root) == 0);
    talloc_free(renamed);
    talloc_free(link); talloc_free(missing); talloc_free(chunk);
    talloc_free(root); talloc_free(key);
    puts("long UTF-8 cache paths preserve publication, coverage, offline reads and cleanup");
}

int main(int argc, char **argv)
{
    mp_time_init();
    if (argc == 6 && !strcmp(argv[1], "--offline-read")) {
        reset_input(); source = argv[3]; input.offline = true;
        int length = atoi(argv[5]);
        struct stream *s = open_cache(argv[2], argv[4], 1024 * 1024);
        assert(stream_get_size(s) == length);
        assert(read_cache(s, 0, length, 'A') == length);
        assert(input.opens == 0);
        free_stream(s);
        puts("native cache read Dart bytes offline");
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--locked")) {
        reset_input(); source = argv[3];
        struct stream *s = talloc_zero(NULL, struct stream);
        s->url = talloc_strdup(s, url);
        struct stream_open_args args = {.url = url};
        struct floe_cache_stream_options options = {
            .enabled = true, .directory = argv[2], .key = argv[4], .url = url,
            .source = source, .max_bytes = 1024 * 1024,
        };
        assert(stream_floe_cache_open(s, &args, &options) == STREAM_NO_MATCH);
        assert(input.opens == 0);
        free_stream(s);
        puts("native cache respects Dart root guard");
        return 0;
    }
    assert(argc == 2);
    base = talloc_asprintf(NULL, "%s/floe-cache-tests-%ld", argv[1], (long)getpid());
    mp_mkdirp(base);
    test_bypass();
    test_offline_and_related();
    test_lock_contention();
    test_changes();
    test_corruption();
    test_quota_and_clear();
    test_uncacheable();
    test_initial_200();
    test_file_paths();
    talloc_free(base);
    puts("all Floe native cache regressions passed");
    return 0;
}
