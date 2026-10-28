/*
 * Floe persistent byte cache for native mpv streams.
 *
 * The cache is intentionally a fail-open wrapper. If its directory, index or
 * a chunk cannot be used, the original stream remains responsible for the
 * read. This keeps cache failures from changing playback availability.
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>

#include "config.h"

#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/hash.h"
#include "misc/io_utils.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "mpv/client.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "player/client.h"
#include "stream.h"
#include "stream_floe_cache.h"

#define FLOE_CACHE_CHUNK_SIZE (4 * 1024 * 1024)

struct floe_cache_entry {
    char *path;
    int64_t start;
    int64_t length;
    int64_t mtime;
};

struct floe_cache_stream {
    stream_t *stream;
    char *directory;
    char *hash;
    char *key;
    int64_t max_bytes;
    int64_t lower_pos;

    int (*lower_fill_buffer)(stream_t *s, void *buffer, int max_len);
    int (*lower_seek)(stream_t *s, int64_t pos);
    int64_t (*lower_get_size)(stream_t *s);
    int (*lower_control)(stream_t *s, int cmd, void *arg);
    void (*lower_close)(stream_t *s);
};

static mp_mutex floe_cache_lock = MP_STATIC_MUTEX_INITIALIZER;

static bool is_floe_chunk_name(const char *name, const char *hash,
                               int64_t *start, int64_t *length)
{
    size_t hash_len = strlen(hash);
    if (strncmp(name, hash, hash_len) != 0 || name[hash_len] != '-')
        return false;

    const char *numbers = name + hash_len + 1;
    char suffix[16];
    if (snprintf(suffix, sizeof(suffix), ".chunk") >= (int)sizeof(suffix))
        return false;

    const char *suffix_pos = strstr(numbers, suffix);
    if (!suffix_pos || suffix_pos[strlen(suffix)] != '\0')
        return false;

    size_t number_len = suffix_pos - numbers;
    char *numbers_copy = malloc(number_len + 1);
    if (!numbers_copy)
        return false;
    memcpy(numbers_copy, numbers, number_len);
    numbers_copy[number_len] = '\0';

    char *separator = strchr(numbers_copy, '-');
    if (!separator) {
        free(numbers_copy);
        return false;
    }

    *separator = '\0';
    char *end = NULL;
    errno = 0;
    int64_t parsed_start = strtoll(numbers_copy, &end, 10);
    if (errno || end == numbers_copy || *end != '\0') {
        free(numbers_copy);
        return false;
    }

    end = NULL;
    errno = 0;
    int64_t parsed_length = strtoll(separator + 1, &end, 10);
    if (errno || end == separator + 1 || *end != '\0' || parsed_start < 0 ||
        parsed_length <= 0) {
        free(numbers_copy);
        return false;
    }

    *start = parsed_start;
    *length = parsed_length;
    free(numbers_copy);
    return true;
}

static struct floe_cache_entry *scan_entries(void *ctx,
                                             struct floe_cache_stream *cache,
                                             int *count)
{
    *count = 0;
    DIR *dir = opendir(cache->directory);
    if (!dir)
        return NULL;

    struct floe_cache_entry *entries = NULL;
    struct dirent *item;
    while ((item = readdir(dir))) {
        int64_t start = 0;
        int64_t length = 0;
        if (!is_floe_chunk_name(item->d_name, cache->hash, &start, &length))
            continue;

        char *path = mp_path_join(ctx, cache->directory, item->d_name);
        struct stat st = {0};
        if (stat(path, &st) != 0 || st.st_size <= 0)
            continue;

        entries = talloc_realloc(ctx, entries, struct floe_cache_entry,
                                 *count + 1);
        entries[*count] = (struct floe_cache_entry){
            .path = path,
            .start = start,
            .length = length,
            .mtime = (int64_t)st.st_mtime,
        };
        (*count)++;
    }
    closedir(dir);
    return entries;
}

static void touch_file(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0)
        return;

    struct utimbuf times = {
        .actime = st.st_atime,
        .modtime = time(NULL),
    };
    utime(path, &times);
}

static char *chunk_path(void *ctx, struct floe_cache_stream *cache,
                        int64_t start, int64_t length)
{
    return talloc_asprintf(ctx, "%s/%s-%" PRId64 "-%" PRId64 ".chunk",
                           cache->directory, cache->hash, start, length);
}

static int entry_mtime_compare(const void *a, const void *b)
{
    const struct floe_cache_entry *ea = a;
    const struct floe_cache_entry *eb = b;
    if (ea->mtime < eb->mtime)
        return -1;
    if (ea->mtime > eb->mtime)
        return 1;
    return strcmp(ea->path, eb->path);
}

static int64_t total_size(struct floe_cache_entry *entries, int count)
{
    int64_t total = 0;
    for (int n = 0; n < count; n++) {
        if (entries[n].length > 0 && INT64_MAX - total >= entries[n].length)
            total += entries[n].length;
    }
    return total;
}

static void evict_if_needed(struct floe_cache_stream *cache)
{
    if (cache->max_bytes <= 0)
        return;

    void *ctx = talloc_new(NULL);
    int count = 0;
    struct floe_cache_entry *entries = scan_entries(ctx, cache, &count);
    if (!entries || count <= 0) {
        talloc_free(ctx);
        return;
    }

    int64_t total = total_size(entries, count);
    if (total <= cache->max_bytes) {
        talloc_free(ctx);
        return;
    }

    qsort(entries, count, sizeof(entries[0]), entry_mtime_compare);
    for (int n = 0; n < count && total > cache->max_bytes; n++) {
        if (unlink(entries[n].path) == 0)
            total -= entries[n].length;
    }
    talloc_free(ctx);
}

static int cache_read(struct floe_cache_stream *cache, int64_t position,
                      void *buffer, int max_len)
{
    if (position < 0 || max_len <= 0)
        return 0;

    void *ctx = talloc_new(NULL);
    int count = 0;
    struct floe_cache_entry *entries = scan_entries(ctx, cache, &count);
    struct floe_cache_entry *selected = NULL;
    for (int n = 0; n < count; n++) {
        if (entries[n].start <= position &&
            position < entries[n].start + entries[n].length &&
            (!selected || entries[n].start > selected->start)) {
            selected = &entries[n];
        }
    }

    if (!selected) {
        talloc_free(ctx);
        return 0;
    }

    int64_t available = selected->start + selected->length - position;
    int amount = (int)MPMIN((int64_t)max_len, available);
    int fd = open(selected->path, O_RDONLY | O_BINARY);
    if (fd < 0 || lseek(fd, position - selected->start, SEEK_SET) < 0) {
        if (fd >= 0)
            close(fd);
        talloc_free(ctx);
        return 0;
    }

    int result = read(fd, buffer, amount);
    close(fd);
    if (result <= 0) {
        unlink(selected->path);
        talloc_free(ctx);
        return 0;
    }

    touch_file(selected->path);
    talloc_free(ctx);
    return result;
}

static void cache_write(struct floe_cache_stream *cache, int64_t start,
                        const void *data, int length)
{
    if (start < 0 || length <= 0 || cache->max_bytes <= 0)
        return;

    const uint8_t *bytes = data;
    int remaining = length;
    int64_t offset = start;
    while (remaining > 0) {
        int part = MPMIN(remaining, FLOE_CACHE_CHUNK_SIZE);
        void *ctx = talloc_new(NULL);
        char *path = chunk_path(ctx, cache, offset, part);
        struct stat st = {0};
        if (stat(path, &st) != 0)
            mp_save_to_file(path, bytes, part);
        touch_file(path);
        talloc_free(ctx);

        bytes += part;
        offset += part;
        remaining -= part;
    }

    evict_if_needed(cache);
}

static int floe_cache_fill_buffer(stream_t *s, void *buffer, int max_len)
{
    struct floe_cache_stream *cache = s->floe_cache;
    int64_t position = s->pos;

    mp_mutex_lock(&floe_cache_lock);
    int cached = cache_read(cache, position, buffer, max_len);
    mp_mutex_unlock(&floe_cache_lock);
    if (cached > 0)
        return cached;

    if (cache->lower_seek && cache->lower_pos != position) {
        if (cache->lower_seek(s, position) <= 0)
            return 0;
        cache->lower_pos = position;
    }

    if (!cache->lower_fill_buffer)
        return 0;
    int result = cache->lower_fill_buffer(s, buffer, max_len);
    if (result <= 0)
        return result;

    mp_mutex_lock(&floe_cache_lock);
    cache_write(cache, position, buffer, result);
    mp_mutex_unlock(&floe_cache_lock);
    cache->lower_pos = position + result;
    return result;
}

static int floe_cache_seek(stream_t *s, int64_t position)
{
    struct floe_cache_stream *cache = s->floe_cache;
    if (!cache->lower_seek)
        return 0;
    int result = cache->lower_seek(s, position);
    if (result > 0)
        cache->lower_pos = position;
    return result;
}

static int64_t floe_cache_get_size(stream_t *s)
{
    struct floe_cache_stream *cache = s->floe_cache;
    return cache->lower_get_size ? cache->lower_get_size(s) : -1;
}

static int floe_cache_control(stream_t *s, int cmd, void *arg)
{
    struct floe_cache_stream *cache = s->floe_cache;
    return cache->lower_control ? cache->lower_control(s, cmd, arg)
                                : STREAM_UNSUPPORTED;
}

static void floe_cache_close(stream_t *s)
{
    struct floe_cache_stream *cache = s->floe_cache;
    if (cache && cache->lower_close)
        cache->lower_close(s);
}

static bool is_http_url(const char *url)
{
    bstr protocol = mp_split_proto(bstr0(url), NULL);
    return bstrcasecmp0(protocol, "http") == 0 ||
           bstrcasecmp0(protocol, "https") == 0;
}

bool stream_floe_cache_wrap(stream_t *s,
                            const struct floe_cache_stream_options *options)
{
    if (!s || !options || !options->enabled || !options->directory ||
        !options->directory[0] || !options->key || !options->key[0] ||
        options->max_bytes <= 0 || s->mode != STREAM_READ ||
        !is_http_url(s->url) || !s->fill_buffer || !s->seek)
        return false;

    struct floe_cache_stream *cache = talloc_zero(s, struct floe_cache_stream);
    cache->stream = s;
    cache->directory = mp_path_join(cache, options->directory, "floe-cache");
    cache->key = talloc_strdup(cache, options->key);
    bstr hash = mp_hash_to_bstr(cache, (const uint8_t *)cache->key,
                                strlen(cache->key), "SHA256");
    cache->hash = bstrto0(cache, hash);
    cache->max_bytes = options->max_bytes;
    cache->lower_fill_buffer = s->fill_buffer;
    cache->lower_seek = s->seek;
    cache->lower_get_size = s->get_size;
    cache->lower_control = s->control;
    cache->lower_close = s->close;
    cache->lower_pos = s->pos;

    mp_mkdirp(cache->directory);
    struct stat st = {0};
    if (stat(cache->directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        talloc_free(cache);
        return false;
    }

    s->floe_cache = cache;
    s->fill_buffer = floe_cache_fill_buffer;
    s->seek = floe_cache_seek;
    s->get_size = floe_cache_get_size;
    s->control = floe_cache_control;
    s->close = floe_cache_close;
    return true;
}

bool stream_floe_cache_get_state(stream_t *s, struct mpv_node *node)
{
    struct floe_cache_stream *cache = s ? s->floe_cache : NULL;
    if (!cache || !node)
        return false;

    void *ctx = talloc_new(NULL);
    int count = 0;
    struct floe_cache_entry *entries = NULL;
    mp_mutex_lock(&floe_cache_lock);
    entries = scan_entries(ctx, cache, &count);
    mp_mutex_unlock(&floe_cache_lock);

    node_init(node, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(node, "key", cache->key);
    node_map_add_string(node, "directory", cache->directory);
    node_map_add_int64(node, "max-bytes", cache->max_bytes);
    int64_t cached_bytes = 0;
    struct mpv_node *ranges = node_map_add(node, "ranges",
                                            MPV_FORMAT_NODE_ARRAY);
    for (int n = 0; n < count; n++) {
        cached_bytes += entries[n].length;
        struct mpv_node *range = node_array_add(ranges,
                                                MPV_FORMAT_NODE_MAP);
        node_map_add_int64(range, "start", entries[n].start);
        node_map_add_int64(range, "end", entries[n].start + entries[n].length);
    }
    node_map_add_int64(node, "cached-bytes", cached_bytes);
    int64_t size = floe_cache_get_size(s);
    if (size >= 0)
        node_map_add_int64(node, "file-size", size);
    talloc_free(ctx);
    return true;
}

