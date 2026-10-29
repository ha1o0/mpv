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
#endif

static inline int floe_cache_lstat(const char *path, struct stat *st)
{
#ifdef _WIN32
    wchar_t *wide = mp_from_utf8(NULL, path);
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
    wchar_t *wide = mp_from_utf8(NULL, path);
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
