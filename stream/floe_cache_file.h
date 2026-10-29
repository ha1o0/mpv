// Cache-local path helpers. Windows uses mpv's UTF-8-aware handles and stat
// structure; POSIX keeps lstat's no-follow behavior. Reparse points are never
// treated as ordinary cache files or directories.
#pragma once

#include <errno.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <utime.h>
#endif
#include "config.h"
#include "mpv_talloc.h"
#include "osdep/io.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>

// libmpv inherits the host process's manifest and long-path policy. Cache
// filenames contain three hashes, so even a short root exceeds MAX_PATH.
// Normalize separators and relative components before adding the extended
// prefix; Win32 treats '/' literally after that prefix.
static inline wchar_t *floe_cache_windows_path(void *ctx, const char *path)
{
    wchar_t *wide = mp_from_utf8(ctx, path);
    for (wchar_t *p = wide; *p; p++) {
        if (*p == L'/') *p = L'\\';
    }
    if (!wcsncmp(wide, L"\\\\?\\", 4)) return wide;

    DWORD count = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!count || count > MP_PATH_MAX - 8) {
        talloc_free(wide);
        errno = count ? ENAMETOOLONG : EINVAL;
        return NULL;
    }
    wchar_t *full = talloc_array(wide, wchar_t, count);
    DWORD length = GetFullPathNameW(wide, count, full, NULL);
    if (!length || length >= count) {
        talloc_free(wide);
        errno = EINVAL;
        return NULL;
    }
    bool unc = full[0] == L'\\' && full[1] == L'\\';
    const wchar_t *prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    size_t prefix_length = wcslen(prefix), skip = unc ? 2 : 0;
    wchar_t *result = talloc_array(ctx, wchar_t, prefix_length + length - skip + 1);
    wmemcpy(result, prefix, prefix_length);
    wmemcpy(result + prefix_length, full + skip, length - skip + 1);
    talloc_free(wide);
    return result;
}

static inline char *floe_cache_windows_utf8_path(const char *path)
{
    wchar_t *wide = floe_cache_windows_path(NULL, path);
    if (!wide) return NULL;
    char *result = mp_to_utf8(NULL, wide);
    talloc_free(wide);
    return result;
}
#endif

// Limit extended-path handling to the cache. POSIX keeps the original paths
// and filesystem calls, including its existing no-follow ownership checks.
static inline int floe_cache_open(const char *path, int flags, int mode)
{
#ifdef _WIN32
    char *native = floe_cache_windows_utf8_path(path);
    if (!native) return -1;
    int result = open(native, flags, mode);
    talloc_free(native);
    return result;
#else
    return open(path, flags, mode);
#endif
}

static inline int floe_cache_mkdir(const char *path, int mode)
{
#ifdef _WIN32
    char *native = floe_cache_windows_utf8_path(path);
    if (!native) return -1;
    int result = mkdir(native, mode);
    talloc_free(native);
    return result;
#else
    return mkdir(path, mode);
#endif
}

static inline DIR *floe_cache_opendir(const char *path)
{
#ifdef _WIN32
    char *native = floe_cache_windows_utf8_path(path);
    if (!native) return NULL;
    DIR *result = opendir(native);
    talloc_free(native);
    return result;
#else
    return opendir(path);
#endif
}

static inline int floe_cache_unlink(const char *path)
{
#ifdef _WIN32
    char *native = floe_cache_windows_utf8_path(path);
    if (!native) return -1;
    int result = unlink(native);
    talloc_free(native);
    return result;
#else
    return unlink(path);
#endif
}

static inline int floe_cache_rename(const char *from, const char *to)
{
#ifdef _WIN32
    char *native_from = floe_cache_windows_utf8_path(from);
    if (!native_from) return -1;
    char *native_to = floe_cache_windows_utf8_path(to);
    int result = native_to ? rename(native_from, native_to) : -1;
    talloc_free(native_to);
    talloc_free(native_from);
    return result;
#else
    return rename(from, to);
#endif
}

static inline int floe_cache_lstat(const char *path, struct stat *st)
{
#ifdef _WIN32
    wchar_t *wide = floe_cache_windows_path(NULL, path);
    if (!wide) return -1;
    HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    talloc_free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        errno = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? ENOENT : error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION
            ? EACCES : EINVAL;
        return -1;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes;
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
        CloseHandle(handle);
        errno = EIO;
        return -1;
    }
    if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // A successful result with neither regular-file nor directory mode
        // makes every existing cache ownership check skip this entry.
        *st = (struct stat){0};
        CloseHandle(handle);
        return 0;
    }
    int descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return -1;
    }
    int result = fstat(descriptor, st);
    close(descriptor); // also closes the transferred Windows handle
    return result;
#else
    return lstat(path, st);
#endif
}

static inline bool floe_cache_path_missing(const char *path)
{
    struct stat st;
    return floe_cache_lstat(path, &st) < 0 && errno == ENOENT;
}

static inline int floe_cache_touch(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = floe_cache_windows_path(NULL, path);
    if (!wide) return -1;
    HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    talloc_free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EACCES;
        return -1;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes;
    bool valid = GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
        &attributes, sizeof(attributes)) &&
        !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    int result = valid && SetFileTime(handle, NULL, &now, &now) ? 0 : -1;
    if (result < 0) errno = EIO;
    CloseHandle(handle);
    return result;
#else
    return utime(path, NULL);
#endif
}
