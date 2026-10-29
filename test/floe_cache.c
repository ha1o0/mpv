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
#include "misc/path_utils.h"
#include "osdep/io.h"
#include "stream/stream.h"
#include "stream/stream_floe_cache.h"

struct input {
    int length, opens, status;
    char fill;
    const char *etag, *control, *type;
    bool offline, ignore_range;
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

struct mp_network_opts *stream_curl_floe_cache_snapshot(void *ctx, struct stream *s)
{
    return NULL;
}
bool stream_curl_floe_cache_allowed(struct mp_network_opts *network) { return true; }

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
int stream_curl_floe_cache_open(struct stream *parent, int64_t start, int64_t end,
    struct mp_network_opts *network, const struct floe_cache_http_response *expected,
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
        .etag = "\"v1\"", .type = "video/mp4", .status = 206};
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
    DIR *dir = opendir(directory);
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
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    int64_t size = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char *path = mp_path_join(NULL, directory, entry->d_name);
        struct stat st;
        assert(lstat(path, &st) == 0);
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
    DIR *dir = opendir(directory);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char *path = mp_path_join(NULL, directory, entry->d_name);
            assert(unlink(path) == 0); talloc_free(path);
        }
        closedir(dir);
    }
    char *epoch = mp_path_join(NULL, root, ".epoch");
    FILE *file = fopen(epoch, "wb"); assert(file);
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
        FILE *file = fopen(chunk, truncated ? "wb" : "r+b"); assert(file);
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

int main(int argc, char **argv)
{
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
    test_offline_and_related();
    test_changes();
    test_corruption();
    test_quota_and_clear();
    test_uncacheable();
    test_initial_200();
    talloc_free(base);
    puts("all Floe native cache regressions passed");
    return 0;
}
