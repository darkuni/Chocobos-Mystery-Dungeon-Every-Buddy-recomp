/* See boot_util.h. */
#define _FILE_OFFSET_BITS 64
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "boot_util.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

FILE *boot_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *p = wide(path), *m = wide(mode);
    FILE *f = p && m ? _wfopen(p, m) : NULL;
    free(p); free(m);
    return f;
#else
    return fopen(path, mode);
#endif
}
int boot_seek(FILE *f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, (long long)offset, SEEK_SET);
#else
    return fseeko(f, (off_t)offset, SEEK_SET);
#endif
}
static int make_dir(const char *path) {
#ifdef _WIN32
    wchar_t *p = wide(path);
    int r = p && (CreateDirectoryW(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
    free(p);
    return r;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}
int boot_mkdir_p(const char *path) {
    char buf[4096];
    if (strlen(path) >= sizeof(buf)) return -1;
    strcpy(buf, path);
    for (char *p = buf + 1; *p; ++p)
        if (*p == '/' || *p == '\\') {
            if (p[-1] == ':') continue; /* C:\ */
            char c = *p; *p = 0;
            if (make_dir(buf)) return -1;
            *p = c;
        }
    return make_dir(buf);
}
int boot_stat(const char *path, uint64_t *size, int64_t *mtime) {
#ifdef _WIN32
    wchar_t *p = wide(path);
    struct _stat64 st;
    int r = p ? _wstat64(p, &st) : -1;
    free(p);
#else
    struct stat st;
    int r = stat(path, &st);
#endif
    if (r) return -1;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return (st.st_mode & S_IFMT) == S_IFREG ? 0 : -1;
}
int boot_is_file(const char *path) { return boot_stat(path, NULL, NULL) == 0; }
int boot_rename(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *f = wide(from), *t = wide(to);
    int r = f && t && MoveFileExW(f, t, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
    free(f); free(t);
    return r;
#else
    return rename(from, to);
#endif
}
int boot_remove(const char *path) {
#ifdef _WIN32
    wchar_t *p = wide(path);
    int r = p && DeleteFileW(p) ? 0 : -1;
    free(p);
    return r;
#else
    return unlink(path);
#endif
}
uint8_t *boot_read_file(const char *path, size_t *size) {
    FILE *f = boot_fopen(path, "rb");
    if (!f) return NULL;
    uint64_t n = 0;
    if (boot_stat(path, &n, NULL) || n > ((uint64_t)1 << 31)) { fclose(f); return NULL; }
    uint8_t *data = malloc(n ? (size_t)n : 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data && size) *size = (size_t)n;
    return data;
}
int boot_write_file(const char *path, const void *data, size_t size) {
    FILE *f = boot_fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok ? 0 : -1;
}
int boot_list_dir(const char *dir, int (*fn)(void *ctx, const char *name), void *ctx) {
#ifdef _WIN32
    char pattern[4096];
    if (!boot_path(pattern, sizeof(pattern), "%s\\*", dir)) return -1;
    wchar_t *p = wide(pattern);
    WIN32_FIND_DATAW found;
    HANDLE h = p ? FindFirstFileW(p, &found) : INVALID_HANDLE_VALUE;
    free(p);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof(name), NULL, NULL) <= 0) continue;
        if (fn(ctx, name)) break;
    } while (FindNextFileW(h, &found));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));) {
        char path[4096];
        if (!boot_path(path, sizeof(path), "%s/%s", dir, e->d_name) || !boot_is_file(path)) continue;
        if (fn(ctx, e->d_name)) break;
    }
    closedir(d);
#endif
    return 0;
}
int boot_path(char *out, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out, size, fmt, args);
    va_end(args);
    return n >= 0 && (size_t)n < size;
}
