/* Floe byte cache. Schema, chunks, epochs and quota are shared with Dart. */
#include <ctype.h>
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
#include <libavutil/random_seed.h>
#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/hash.h"
#include "misc/json.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "mpv/client.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "stream/stream.h"
#include "stream/stream_floe_cache.h"

#if HAVE_WIN32_DESKTOP
#include <windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

struct cache_lock { int guard, legacy; };
struct cache_file {
    char *path;
    int64_t size, modified;
    bool pending;
};
struct cache_chunk {
    int64_t start, length;
    char version[65], checksum[65];
};
struct floe_cache_stream {
    struct stream *stream, *lower;
    struct mp_network_opts *network;
    char *root, *source_dir, *directory, *key, *source;
    int64_t max_bytes, lower_end, pending_start, known_length;
    struct floe_cache_http_response metadata;
    char version[65], root_epoch[65], source_epoch[65];
    uint8_t *pending;
    int pending_length;
    uint8_t *read_buffer;
    char *read_path;
    int64_t read_start;
    int read_length;
    bool publish, failed;
};

static mp_mutex floe_cache_lock = MP_STATIC_MUTEX_INITIALIZER;

static bool safe_text(const char *s, size_t maximum)
{
    return s && s[0] && strlen(s) <= maximum && !strchr(s, '\r') && !strchr(s, '\n');
}

static bool hex_string(const char *s, bool uppercase)
{
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (uppercase && s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}

static bool source_name(const char *s)
{
    if (!s) return false;
    if (!strcmp(s, "direct")) return true;
    if (!strncmp(s, "source-", 7)) return hex_string(s + 7, false);
    if (strlen(s) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}

static bool number(const char **p, int64_t *out)
{
    if (**p < '0' || **p > '9') return false;
    char *end;
    errno = 0;
    long long value = strtoll(*p, &end, 10);
    if (errno || end == *p || value < 0) return false;
    *out = value;
    *p = end;
    return true;
}

static bool copy_field(char *out, size_t capacity, const char *value)
{
    if (!safe_text(value, capacity - 1)) return false;
    memcpy(out, value, strlen(value) + 1);
    return true;
}

void stream_floe_cache_parse_http(struct floe_cache_http_response *out,
    int status, int64_t content_length, const char *content_range,
    const char *accept_ranges, const char *etag, const char *modified,
    const char *content_type, const char *encoding,
    const char *cache_control, const char *vary)
{
    *out = (struct floe_cache_http_response){.status = status, .length = -1};
    if (status == 206 && content_range && !strncmp(content_range, "bytes ", 6)) {
        const char *p = content_range + 6;
        int64_t last;
        if (!number(&p, &out->start) || *p++ != '-' || !number(&p, &last) ||
            *p++ != '/' || !number(&p, &out->length) || *p ||
            last == INT64_MAX || last < out->start || last >= out->length)
            return;
        out->end = last + 1;
        if (content_length >= 0 && content_length != out->end - out->start)
            return;
    } else if (status == 200 && content_length > 0) {
        out->length = out->end = content_length;
        if (!accept_ranges || strcasecmp(accept_ranges, "bytes")) return;
    } else {
        return;
    }
    if (out->length <= 0 || (encoding && encoding[0]) ||
        (vary && strchr(vary, '*'))) return;
    if (cache_control) {
        for (const char *p = cache_control; *p; p++)
            if (!strncasecmp(p, "no-store", 8)) return;
    }
    if (content_type && content_type[0]) {
        if (!copy_field(out->content_type, sizeof(out->content_type), content_type))
            return;
        char type[sizeof(out->content_type)];
        for (size_t i = 0; i <= strlen(content_type); i++)
            type[i] = tolower((unsigned char)content_type[i]);
        if (!strncmp(type, "text/", 5) || !strncmp(type, "multipart/", 10) ||
            strstr(type, "mpegurl") || strstr(type, "dash+xml") ||
            strstr(type, "json") || strstr(type, "xml")) return;
    }
    if (safe_text(etag, sizeof(out->validator) - 1) && etag[0] == '"' &&
        etag[strlen(etag) - 1] == '"') {
        strcpy(out->validator, etag);
        strcpy(out->validator_header, "etag");
    } else if (copy_field(out->validator, sizeof(out->validator), modified)) {
        strcpy(out->validator_header, "last-modified");
    } else {
        return;
    }
    out->cacheable = true;
}

bool stream_floe_cache_validate_http(const struct floe_cache_http_response *out,
    const struct floe_cache_http_response *expected, int64_t start, int64_t end)
{
    return out->cacheable && out->status == 206 && out->start == start &&
        out->end > start && out->end <= (end > 0 ? end : out->length) &&
        out->length == expected->length &&
        !strcmp(out->validator_header, expected->validator_header) &&
        !strcmp(out->validator, expected->validator);
}

static void sha256(char out[65], const void *data, size_t size)
{
    void *ctx = talloc_new(NULL);
    bstr hash = mp_hash_to_bstr(ctx, data, size, "SHA256");
    for (int i = 0; i < 64; i++) out[i] = tolower(hash.start[i]);
    out[64] = 0;
    talloc_free(ctx);
}

static bool directory_exists(const char *path, bool create)
{
    struct stat st;
    if (lstat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return create && errno == ENOENT && mkdir(path, 0700) == 0;
}

static char *read_file(void *ctx, const char *path, int maximum, int *length)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > maximum) return NULL;
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return NULL;
    int size = st.st_size, used = 0;
    char *data = talloc_size(ctx, size + 1);
    while (used < size) {
        int amount = read(fd, data + used, size - used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) break;
        used += amount;
    }
    close(fd);
    if (used != size) { talloc_free(data); return NULL; }
    data[size] = 0;
    if (length) *length = size;
    return data;
}

static bool write_atomic(const char *path, const void *data, int size)
{
    char *pending = talloc_asprintf(NULL, "%s.pending", path);
    struct stat st;
    bool regular = lstat(pending, &st) != 0 ? errno == ENOENT : S_ISREG(st.st_mode);
    int fd = regular ? open(pending, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600) : -1;
    bool ok = fd >= 0;
    for (int offset = 0; ok && offset < size;) {
        int amount = write(fd, (const char *)data + offset, size - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { ok = false; break; }
        offset += amount;
    }
    if (fd >= 0) {
#if HAVE_WIN32_DESKTOP
        if (_commit(fd) != 0) ok = false;
#else
        if (fsync(fd) != 0) ok = false;
#endif
        if (close(fd) != 0) ok = false;
    }
    if (ok) ok = rename(pending, path) == 0;
    if (!ok && fd >= 0) unlink(pending);
    talloc_free(pending);
    return ok;
}

static bool lock_descriptor(int fd, bool primary)
{
#if HAVE_WIN32_DESKTOP
    OVERLAPPED position = {0};
    return LockFileEx((HANDLE)_get_osfhandle(fd),
        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
        0, MAXDWORD, MAXDWORD, &position);
#else
    if (primary) return flock(fd, LOCK_EX | LOCK_NB) == 0;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    return fcntl(fd, F_SETLK, &lock) == 0;
#endif
}

static void unlock_descriptor(int fd, bool primary)
{
    if (fd < 0) return;
#if HAVE_WIN32_DESKTOP
    OVERLAPPED position = {0};
    UnlockFileEx((HANDLE)_get_osfhandle(fd), 0, MAXDWORD, MAXDWORD, &position);
#else
    if (primary) flock(fd, LOCK_UN);
    else {
        struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
        fcntl(fd, F_SETLK, &lock);
    }
#endif
    close(fd);
}

static bool root_lock(struct floe_cache_stream *cache, struct cache_lock *lock)
{
    *lock = (struct cache_lock){.guard = -1, .legacy = -1};
    if (mp_mutex_trylock(&floe_cache_lock) != 0) return false;
    void *ctx = talloc_new(NULL);
    char *guard = mp_path_join(ctx, cache->root, ".lock-v2");
    char *legacy = mp_path_join(ctx, cache->root, ".lock");
    struct stat st;
    bool safe = lstat(guard, &st) != 0 ? errno == ENOENT : S_ISREG(st.st_mode);
    if (safe) lock->guard = open(guard, O_RDWR | O_CREAT | O_BINARY, 0600);
    if (lock->guard < 0 || !lock_descriptor(lock->guard, true)) goto failed;
    safe = lstat(legacy, &st) != 0 ? errno == ENOENT : S_ISREG(st.st_mode);
    if (safe) lock->legacy = open(legacy, O_RDWR | O_CREAT | O_BINARY, 0600);
    if (lock->legacy < 0 || !lock_descriptor(lock->legacy, false)) goto failed;
    talloc_free(ctx);
    return true;
failed:
    unlock_descriptor(lock->legacy, false);
    unlock_descriptor(lock->guard, true);
    talloc_free(ctx);
    mp_mutex_unlock(&floe_cache_lock);
    return false;
}

static void root_unlock(struct cache_lock *lock)
{
    unlock_descriptor(lock->legacy, false);
    unlock_descriptor(lock->guard, true);
    mp_mutex_unlock(&floe_cache_lock);
}

static bool epoch(const char *directory, char out[65], bool create)
{
    void *ctx = talloc_new(NULL);
    char *path = mp_path_join(ctx, directory, ".epoch");
    char *data = read_file(ctx, path, 64, NULL);
    bool ok = data && hex_string(data, false);
    if (ok) strcpy(out, data);
    else if (!data && create && access(path, F_OK) != 0 && errno == ENOENT) {
        for (int i = 0; i < 8; i++)
            snprintf(out + i * 8, 9, "%08" PRIx32, av_get_random_seed());
        ok = write_atomic(path, out, 64);
    }
    talloc_free(ctx);
    return ok;
}

static bool valid_lease(struct floe_cache_stream *cache)
{
    char root[65], source[65];
    return epoch(cache->root, root, false) &&
        epoch(cache->source_dir, source, false) &&
        !strcmp(root, cache->root_epoch) && !strcmp(source, cache->source_epoch);
}

static void content_version(char out[65], const struct floe_cache_http_response *metadata)
{
    struct mpv_node value;
    node_init(&value, MPV_FORMAT_NODE_ARRAY, NULL);
    node_array_add(&value, MPV_FORMAT_INT64)->u.int64 = metadata->length;
    struct mpv_node *header = node_array_add(&value, MPV_FORMAT_NONE);
    header->format = MPV_FORMAT_STRING;
    header->u.string = talloc_strdup(value.u.list, metadata->validator_header);
    struct mpv_node *validator = node_array_add(&value, MPV_FORMAT_NONE);
    validator->format = MPV_FORMAT_STRING;
    validator->u.string = talloc_strdup(value.u.list, metadata->validator);
    char *json = NULL;
    json_write(&json, &value);
    sha256(out, json, strlen(json));
    talloc_free(json);
    talloc_free(value.u.list);
}

static bool load_metadata(struct floe_cache_stream *cache,
                          struct floe_cache_http_response *out)
{
    void *ctx = talloc_new(NULL);
    char *path = talloc_asprintf(ctx, "%s/%s.meta", cache->directory, cache->key);
    int size;
    char *data = read_file(ctx, path, 8192, &size), *cursor = data;
    struct mpv_node node;
    bool ok = data && (int)strlen(data) == size &&
        json_parse(ctx, &node, &cursor, 5) == 0;
    if (ok) {
        json_skip_whitespace(&cursor);
        ok = !*cursor && node.format == MPV_FORMAT_NODE_MAP;
    }
    struct mpv_node *schema = ok ? node_map_get(&node, "schema") : NULL;
    struct mpv_node *length = ok ? node_map_get(&node, "length") : NULL;
    struct mpv_node *validator = ok ? node_map_get(&node, "validator") : NULL;
    struct mpv_node *header = ok ? node_map_get(&node, "validatorHeader") : NULL;
    struct mpv_node *type = ok ? node_map_get(&node, "contentType") : NULL;
    *out = (struct floe_cache_http_response){0};
    ok = schema && schema->format == MPV_FORMAT_INT64 && schema->u.int64 == 1 &&
        length && length->format == MPV_FORMAT_INT64 && length->u.int64 > 0 &&
        validator && validator->format == MPV_FORMAT_STRING &&
        copy_field(out->validator, sizeof(out->validator), validator->u.string) &&
        header && header->format == MPV_FORMAT_STRING &&
        copy_field(out->validator_header, sizeof(out->validator_header), header->u.string) &&
        (!strcmp(out->validator_header, "etag") || !strcmp(out->validator_header, "last-modified")) &&
        (!type || type->format == MPV_FORMAT_NONE ||
         (type->format == MPV_FORMAT_STRING &&
          copy_field(out->content_type, sizeof(out->content_type), type->u.string)));
    if (ok) {
        out->length = out->end = length->u.int64;
        out->cacheable = true;
    }
    talloc_free(ctx);
    return ok;
}

static bool parse_chunk(const char *name, const char *key, struct cache_chunk *out)
{
    if (strlen(name) < 64 + 1 + 64 + 1 + 1 + 1 + 1 + 1 + 64 + 6 ||
        strncmp(name, key, 64) || name[64] != '.') return false;
    memcpy(out->version, name + 65, 64);
    out->version[64] = 0;
    if (!hex_string(out->version, false) || name[129] != '.') return false;
    const char *p = name + 130;
    if (!number(&p, &out->start) || *p++ != '.' || !number(&p, &out->length) ||
        *p++ != '.' || out->length <= 0 || out->length > FLOE_CACHE_CHUNK_SIZE ||
        out->start > INT64_MAX - out->length || strlen(p) < 70) return false;
    memcpy(out->checksum, p, 64);
    out->checksum[64] = 0;
    return hex_string(out->checksum, false) && !strcmp(p + 64, ".chunk");
}

static bool owned_file(const char *name, bool legacy)
{
    size_t length = strlen(name);
    bool pending = length > 8 && !strcmp(name + length - 8, ".pending");
    char *base = talloc_strndup(NULL, name, length - (pending ? 8 : 0));
    bool ok = false;
    if (strlen(base) >= 64) {
        char key[65];
        memcpy(key, base, 64); key[64] = 0;
        if (legacy && hex_string(key, true) && base[64] == '-') {
            const char *p = base + 65;
            int64_t start, size;
            ok = number(&p, &start) && *p++ == '-' && number(&p, &size) &&
                size > 0 && !strcmp(p, ".chunk");
        } else if (!legacy && hex_string(key, false)) {
            struct cache_chunk chunk;
            ok = !strcmp(base + 64, ".meta") || parse_chunk(base, key, &chunk);
        }
    }
    talloc_free(base);
    return ok;
}

static bool collect_leaf(void *ctx, const char *directory, bool legacy,
                         struct cache_file **files, int *count)
{
    if (!directory_exists(directory, false)) return true;
    DIR *dir = opendir(directory);
    if (!dir) return false;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (!owned_file(item->d_name, legacy)) continue;
        char *path = mp_path_join(ctx, directory, item->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) {
            if (errno == ENOENT) continue;
            closedir(dir); return false;
        }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) continue;
        *files = talloc_realloc(ctx, *files, struct cache_file, *count + 1);
        (*files)[(*count)++] = (struct cache_file){
            .path = path, .size = st.st_size, .modified = st.st_mtime,
            .pending = strlen(item->d_name) > 8 &&
                !strcmp(item->d_name + strlen(item->d_name) - 8, ".pending"),
        };
    }
    closedir(dir);
    return true;
}

static bool collect_files(void *ctx, const char *root, struct cache_file **files,
                          int *count)
{
    *files = NULL; *count = 0;
    DIR *dir = opendir(root);
    if (!dir) return false;
    struct dirent *item;
    bool ok = true;
    while (ok && (item = readdir(dir))) {
        char *source = mp_path_join(ctx, root, item->d_name);
        if (!strcmp(item->d_name, "floe-cache")) {
            ok = collect_leaf(ctx, source, true, files, count);
            continue;
        }
        if (!source_name(item->d_name) || !directory_exists(source, false)) continue;
        DIR *children = opendir(source);
        if (!children) { ok = false; break; }
        struct dirent *child;
        while (ok && (child = readdir(children))) {
            if (!hex_string(child->d_name, false)) continue;
            char *media = mp_path_join(ctx, source, child->d_name);
            ok = collect_leaf(ctx, media, false, files, count);
        }
        closedir(children);
    }
    closedir(dir);
    return ok;
}

static int compare_modified(const void *a, const void *b)
{
    const struct cache_file *left = a, *right = b;
    if (left->modified != right->modified)
        return left->modified < right->modified ? -1 : 1;
    return strcmp(left->path, right->path);
}

static int64_t current_quota(struct floe_cache_stream *cache)
{
    void *ctx = talloc_new(NULL);
    char *path = mp_path_join(ctx, cache->root, ".quota");
    char *data = read_file(ctx, path, 32, NULL);
    const char *p = data;
    int64_t quota;
    bool ok = data && number(&p, &quota) && !*p;
    bool missing = access(path, F_OK) != 0 && errno == ENOENT;
    talloc_free(ctx);
    return ok ? quota : missing ? cache->max_bytes : 0;
}

static bool make_room(struct floe_cache_stream *cache, int64_t extra,
                       const char *protected_path)
{
    int64_t limit = current_quota(cache);
    if (limit <= 0 || extra < 0 || extra > limit) return false;
    void *ctx = talloc_new(NULL);
    struct cache_file *files;
    int count;
    bool ok = collect_files(ctx, cache->root, &files, &count);
    int64_t total = 0;
    for (int i = 0; ok && i < count; i++) {
        if (files[i].pending) {
            ok = unlink(files[i].path) == 0;
            files[i].size = 0;
        }
        if (files[i].size > INT64_MAX - total) ok = false;
        else total += files[i].size;
    }
    if (ok && total > limit - extra) {
        qsort(files, count, sizeof(*files), compare_modified);
        int64_t target = MPMAX(0, limit - extra - MPMIN(limit / 20, FLOE_CACHE_CHUNK_SIZE * 8));
        for (int i = 0; ok && i < count && total > target; i++) {
            if (files[i].pending || (protected_path && !strcmp(files[i].path, protected_path))) continue;
            if (unlink(files[i].path) != 0) { ok = false; break; }
            total -= files[i].size;
        }
    }
    ok = ok && total <= limit - extra;
    talloc_free(ctx);
    return ok;
}

static char *metadata_path(void *ctx, struct floe_cache_stream *cache)
{
    return talloc_asprintf(ctx, "%s/%s.meta", cache->directory, cache->key);
}

static bool current_version(struct floe_cache_stream *cache)
{
    struct floe_cache_http_response metadata;
    char version[65];
    if (!load_metadata(cache, &metadata)) return false;
    content_version(version, &metadata);
    return !strcmp(cache->version, version);
}

static void remove_media(struct floe_cache_stream *cache)
{
    if (!directory_exists(cache->directory, false)) return;
    DIR *dir = opendir(cache->directory);
    if (!dir) return;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (strncmp(item->d_name, cache->key, 64) || !owned_file(item->d_name, false))
            continue;
        char *path = mp_path_join(NULL, cache->directory, item->d_name);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) unlink(path);
        talloc_free(path);
    }
    closedir(dir);
}

static void invalidate(struct floe_cache_stream *cache)
{
    struct cache_lock lock;
    if (root_lock(cache, &lock)) {
        if (valid_lease(cache) && current_version(cache)) remove_media(cache);
        root_unlock(&lock);
    }
    cache->publish = false;
    cache->failed = true;
    cache->pending_length = 0;
}

static bool register_metadata(struct floe_cache_stream *cache)
{
    struct cache_lock lock;
    if (!root_lock(cache, &lock)) return false;
    bool ok = valid_lease(cache);
    struct floe_cache_http_response existing;
    if (ok && load_metadata(cache, &existing)) {
        char version[65]; content_version(version, &existing);
        ok = !strcmp(version, cache->version);
        root_unlock(&lock);
        return ok;
    }
    void *ctx = talloc_new(NULL);
    struct mpv_node node;
    node_init(&node, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_int64(&node, "schema", 1);
    node_map_add_int64(&node, "length", cache->metadata.length);
    node_map_add_string(&node, "validator", cache->metadata.validator);
    node_map_add_string(&node, "validatorHeader", cache->metadata.validator_header);
    if (cache->metadata.content_type[0])
        node_map_add_string(&node, "contentType", cache->metadata.content_type);
    else node_map_add(&node, "contentType", MPV_FORMAT_NONE);
    char *json = NULL;
    json_write(&json, &node);
    char *path = metadata_path(ctx, cache);
    ok = ok && directory_exists(cache->directory, true) &&
        strlen(json) <= 8192 && make_room(cache, strlen(json) + 8192, path);
    if (ok) {
        remove_media(cache);
        ok = write_atomic(path, json, strlen(json));
    }
    talloc_free(node.u.list); talloc_free(json); talloc_free(ctx);
    root_unlock(&lock);
    return ok;
}

static bool write_chunk(struct floe_cache_stream *cache, int64_t start,
                         const uint8_t *data, int size)
{
    if (size <= 0 || size > FLOE_CACHE_CHUNK_SIZE || start < 0 ||
        start > cache->metadata.length - size) return false;
    char checksum[65]; sha256(checksum, data, size);
    struct cache_lock lock;
    if (!root_lock(cache, &lock)) return false;
    void *ctx = talloc_new(NULL);
    char *path = talloc_asprintf(ctx, "%s/%s.%s.%" PRId64 ".%d.%s.chunk",
        cache->directory, cache->key, cache->version, start, size, checksum);
    char *meta = metadata_path(ctx, cache);
    bool ok = valid_lease(cache) && current_version(cache);
    struct stat st;
    if (ok && lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == size) {
        root_unlock(&lock); talloc_free(ctx); return true;
    }
    ok = ok && directory_exists(cache->directory, false) && make_room(cache, size, meta);
    if (ok) {
        ok = write_atomic(path, data, size);
        if (ok) utime(meta, NULL);
    }
    talloc_free(ctx); root_unlock(&lock);
    return ok;
}

static void flush_pending(struct floe_cache_stream *cache)
{
    if (cache->publish && cache->pending_length)
        cache->publish = write_chunk(cache, cache->pending_start,
                                      cache->pending, cache->pending_length);
    cache->pending_length = 0;
}

static void record_bytes(struct floe_cache_stream *cache, int64_t start,
                         const uint8_t *data, int size)
{
    if (!cache->publish || start < 0 || start > cache->metadata.length - size)
        return;
    if (cache->pending_length &&
        start != cache->pending_start + cache->pending_length) flush_pending(cache);
    int used = 0;
    while (cache->publish && used < size) {
        if (!cache->pending_length) cache->pending_start = start + used;
        int count = MPMIN(FLOE_CACHE_CHUNK_SIZE - cache->pending_length, size - used);
        if (!cache->pending) cache->pending = talloc_size(cache, FLOE_CACHE_CHUNK_SIZE);
        memcpy(cache->pending + cache->pending_length, data + used, count);
        cache->pending_length += count; used += count;
        if (cache->pending_length == FLOE_CACHE_CHUNK_SIZE) flush_pending(cache);
    }
    if (start + size == cache->metadata.length) flush_pending(cache);
}

static int read_cached(struct floe_cache_stream *cache, int64_t position,
                        uint8_t *buffer, int maximum)
{
    if (!cache->metadata.cacheable) return 0;
    struct cache_lock lock;
    if (!root_lock(cache, &lock)) return 0;
    if (!current_version(cache)) { root_unlock(&lock); return 0; }
    struct stat st;
    bool in_memory = cache->read_path && cache->read_start <= position &&
        position < cache->read_start + cache->read_length &&
        lstat(cache->read_path, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size == cache->read_length;
    if (!in_memory) {
        DIR *dir = opendir(cache->directory);
        if (!dir) { root_unlock(&lock); return 0; }
        struct dirent *item;
        struct cache_chunk chosen = {0}, chunk;
        char *path = NULL;
        while ((item = readdir(dir))) {
            if (!parse_chunk(item->d_name, cache->key, &chunk) ||
                strcmp(chunk.version, cache->version) || chunk.start > position ||
                position >= chunk.start + chunk.length ||
                chunk.start + chunk.length > cache->metadata.length) continue;
            if (!path || chunk.start + chunk.length > chosen.start + chosen.length) {
                char *candidate = mp_path_join(NULL, cache->directory, item->d_name);
                struct stat candidate_stat;
                if (lstat(candidate, &candidate_stat) != 0 || !S_ISREG(candidate_stat.st_mode)) {
                    talloc_free(candidate); continue;
                }
                talloc_free(path);
                path = candidate;
                chosen = chunk;
            }
        }
        closedir(dir);
        if (!path) { root_unlock(&lock); return 0; }
        void *ctx = talloc_new(NULL);
        int length = 0;
        char *bytes = read_file(ctx, path, FLOE_CACHE_CHUNK_SIZE, &length);
        char checksum[65] = {0};
        if (bytes) sha256(checksum, bytes, length);
        if (!bytes || length != chosen.length || strcmp(checksum, chosen.checksum)) {
            if (valid_lease(cache)) unlink(path);
            talloc_free(path); talloc_free(ctx); root_unlock(&lock); return 0;
        }
        if (!cache->read_buffer) cache->read_buffer = talloc_size(cache, FLOE_CACHE_CHUNK_SIZE);
        memcpy(cache->read_buffer, bytes, length);
        talloc_free(cache->read_path);
        cache->read_path = talloc_steal(cache, path);
        cache->read_start = chosen.start; cache->read_length = length;
        talloc_free(ctx);
    }
    int amount = MPMIN((int64_t)maximum,
        cache->read_start + cache->read_length - position);
    memcpy(buffer, cache->read_buffer + position - cache->read_start, amount);
    if (valid_lease(cache) && lstat(cache->read_path, &st) == 0 &&
        time(NULL) - st.st_mtime >= 30) {
        utime(cache->read_path, NULL);
        char *meta = metadata_path(NULL, cache);
        utime(meta, NULL); talloc_free(meta);
    }
    root_unlock(&lock);
    return amount;
}

static int64_t next_cached_start(struct floe_cache_stream *cache, int64_t position)
{
    int64_t end = cache->metadata.length > 0 ? cache->metadata.length : 0;
    struct cache_lock lock;
    if (!cache->metadata.cacheable || !root_lock(cache, &lock)) return end;
    DIR *dir = opendir(cache->directory);
    if (dir) {
        struct dirent *item;
        struct cache_chunk chunk;
        while ((item = readdir(dir))) {
            if (parse_chunk(item->d_name, cache->key, &chunk) &&
                !strcmp(chunk.version, cache->version) && chunk.start > position &&
                chunk.start < end) end = chunk.start;
        }
        closedir(dir);
    }
    root_unlock(&lock);
    return end;
}

static void close_lower(struct floe_cache_stream *cache)
{
    flush_pending(cache);
    if (cache->lower) free_stream(cache->lower);
    cache->lower = NULL;
    cache->lower_end = 0;
}

static bool open_lower(struct floe_cache_stream *cache, int64_t position,
                        bool initial)
{
#if HAVE_LIBCURL
    struct floe_cache_http_response response = {0};
    const struct floe_cache_http_response *expected =
        cache->metadata.cacheable ? &cache->metadata : NULL;
    int64_t end = expected ? next_cached_start(cache, position) : 0;
    int result = stream_curl_floe_cache_open(cache->stream, position, end,
        cache->network, expected, &cache->lower, &response);
    if (response.changed) { invalidate(cache); close_lower(cache); return false; }
    if (result != STREAM_OK) return false;
    if (expected && !stream_floe_cache_validate_http(&response, expected, position, end)) {
        invalidate(cache); close_lower(cache); return false;
    }
    if (initial) {
        cache->metadata = response;
        if (response.cacheable) {
            content_version(cache->version, &response);
            cache->publish = register_metadata(cache);
        }
    }
    cache->lower_end = cache->lower->seekable ? response.end : 0;
    return true;
#else
    return false;
#endif
}

static bool lower_changed(struct floe_cache_stream *cache)
{
    struct floe_cache_http_response response = {0};
    if (stream_control(cache->lower, STREAM_CTRL_GET_FLOE_HTTP_RESPONSE, &response) == STREAM_OK &&
        response.changed) {
        invalidate(cache); close_lower(cache); return true;
    }
    return false;
}

static int cache_fill_buffer(struct stream *s, void *buffer, int maximum)
{
    struct floe_cache_stream *cache = s->floe_cache;
    if (cache->failed || maximum <= 0) return 0;
    int64_t position = s->pos;
    if (cache->known_length >= 0 && position >= cache->known_length) {
        close_lower(cache); return 0;
    }
    int hit = read_cached(cache, position, buffer, maximum);
    if (hit > 0) { close_lower(cache); return hit; }
    if (cache->lower && (stream_tell(cache->lower) != position ||
        (cache->lower_end > 0 && position >= cache->lower_end))) close_lower(cache);
    if (!cache->lower && !open_lower(cache, position, false)) return 0;
    if (lower_changed(cache)) return 0;
    if (cache->lower_end > 0)
        maximum = MPMIN((int64_t)maximum, cache->lower_end - position);
    int amount = stream_read_partial(cache->lower, buffer, maximum);
    if (lower_changed(cache)) return 0;
    if (amount > 0) record_bytes(cache, position, buffer, amount);
    else flush_pending(cache);
    return amount;
}

static int cache_seek(struct stream *s, int64_t position)
{
    struct floe_cache_stream *cache = s->floe_cache;
    if (cache->failed || position < 0 ||
        (cache->metadata.length > 0 && position > cache->metadata.length)) return 0;
    // Cached ranges and the resource size need no network operation. Open the
    // lower stream only if the next read actually encounters a gap.
    close_lower(cache);
    return 1;
}

static int64_t cache_get_size(struct stream *s)
{
    struct floe_cache_stream *cache = s->floe_cache;
    if (cache->known_length >= 0) return cache->known_length;
    return cache->lower ? stream_get_size(cache->lower) : cache->metadata.length;
}

static int cache_control(struct stream *s, int command, void *arg)
{
    struct floe_cache_stream *cache = s->floe_cache;
    return cache->lower ? stream_control(cache->lower, command, arg) : STREAM_UNSUPPORTED;
}

static void cache_close(struct stream *s)
{
    close_lower(s->floe_cache);
}

static void cache_destructor(void *arg)
{
    close_lower(arg);
}

int stream_floe_cache_version(void)
{
    return HAVE_LIBCURL ? 2 : 0;
}

int stream_floe_cache_open(struct stream *s, struct stream_open_args *args,
                          const struct floe_cache_stream_options *options)
{
    if (!stream_floe_cache_version() || !options || !options->enabled ||
        !options->directory || !options->directory[0] || options->max_bytes <= 0 ||
        !hex_string(options->key, false) || !options->url ||
        strcmp(options->url, s->url) || args->no_floe_cache || args->special_arg ||
        s->mode != STREAM_READ ||
        (strncmp(s->url, "http://", 7) && strncmp(s->url, "https://", 8)))
        return STREAM_NO_MATCH;
    const char *source = options->source && options->source[0] ? options->source : "direct";
    if (!source_name(source)) return STREAM_NO_MATCH;
    struct floe_cache_stream *cache = talloc_zero(s, struct floe_cache_stream);
    cache->stream = s;
    cache->root = talloc_strdup(cache, options->directory);
    cache->source = talloc_strdup(cache, source);
    cache->key = talloc_strdup(cache, options->key);
    cache->source_dir = mp_path_join(cache, cache->root, cache->source);
    cache->directory = mp_path_join(cache, cache->source_dir, cache->key);
    cache->max_bytes = options->max_bytes;
    cache->known_length = -1;
#if HAVE_LIBCURL
    cache->network = stream_curl_floe_cache_snapshot(cache, s);
    if (!stream_curl_floe_cache_allowed(cache->network)) {
        talloc_free(cache); return STREAM_NO_MATCH;
    }
#endif
    mp_mkdirp(cache->root);
    struct cache_lock lock;
    if (!directory_exists(cache->root, false) || !root_lock(cache, &lock)) {
        talloc_free(cache); return STREAM_NO_MATCH;
    }
    bool ready = directory_exists(cache->source_dir, true) &&
        epoch(cache->root, cache->root_epoch, true) &&
        epoch(cache->source_dir, cache->source_epoch, true);
    char *quota_path = mp_path_join(NULL, cache->root, ".quota");
    if (ready && access(quota_path, F_OK) != 0 && errno == ENOENT) {
        char value[32];
        int size = snprintf(value, sizeof(value), "%" PRId64, cache->max_bytes);
        ready = write_atomic(quota_path, value, size);
    }
    talloc_free(quota_path);
    bool loaded = ready && load_metadata(cache, &cache->metadata);
    if (loaded) {
        content_version(cache->version, &cache->metadata);
        cache->publish = true;
    }
    root_unlock(&lock);
    if (!ready) { talloc_free(cache); return STREAM_NO_MATCH; }
    talloc_set_destructor(cache, cache_destructor);
    if (!loaded && !open_lower(cache, 0, true)) {
        talloc_free(cache); return STREAM_ERROR;
    }
    s->floe_cache = cache;
    cache->known_length = cache->metadata.length > 0 &&
        (loaded || cache->lower->seekable) ? cache->metadata.length : stream_get_size(cache->lower);
    s->is_network = s->streaming = s->fast_skip = true;
    s->seekable = loaded || cache->lower->seekable;
    if (cache->metadata.content_type[0]) {
        bstr mime = bstr_split(bstr0(cache->metadata.content_type), ";", NULL);
        s->mime_type = bstrto0(s, bstr_strip(mime));
    } else if (cache->lower && cache->lower->mime_type) {
        s->mime_type = talloc_strdup(s, cache->lower->mime_type);
    }
    s->fill_buffer = cache_fill_buffer;
    s->seek = s->seekable ? cache_seek : NULL;
    s->get_size = cache_get_size;
    s->control = cache_control;
    s->close = cache_close;
    return STREAM_OK;
}

bool stream_floe_cache_get_state(struct stream *s, struct mpv_node *node)
{
    struct floe_cache_stream *cache = s ? s->floe_cache : NULL;
    if (!cache || !node || !cache->metadata.cacheable) return false;
    struct cache_lock lock;
    if (!root_lock(cache, &lock)) return false;
    node_init(node, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(node, "key", cache->key);
    node_map_add_string(node, "directory", cache->directory);
    node_map_add_int64(node, "max-bytes", current_quota(cache));
    node_map_add_int64(node, "file-size", cache->metadata.length);
    struct mpv_node *ranges = node_map_add(node, "ranges", MPV_FORMAT_NODE_ARRAY);
    int64_t size = 0;
    DIR *dir = current_version(cache) ? opendir(cache->directory) : NULL;
    struct dirent *item;
    struct cache_chunk chunk;
    if (dir) {
        while ((item = readdir(dir))) {
            if (!parse_chunk(item->d_name, cache->key, &chunk) ||
                strcmp(chunk.version, cache->version) ||
                chunk.start + chunk.length > cache->metadata.length) continue;
            char *path = mp_path_join(NULL, cache->directory, item->d_name);
            struct stat st;
            bool valid = lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == chunk.length;
            talloc_free(path);
            if (!valid || size > INT64_MAX - chunk.length) continue;
            struct mpv_node *range = node_array_add(ranges, MPV_FORMAT_NODE_MAP);
            node_map_add_int64(range, "start", chunk.start);
            node_map_add_int64(range, "end", chunk.start + chunk.length);
            size += chunk.length;
        }
        closedir(dir);
    }
    node_map_add_int64(node, "cached-bytes", size);
    root_unlock(&lock);
    return true;
}
