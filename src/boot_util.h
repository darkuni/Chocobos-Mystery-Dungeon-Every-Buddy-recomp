/* File helpers for the first-start steps (bootstrap.c, boot_pkg.c, boot_prepare.c): UTF-8 paths
 * on every platform, 64-bit offsets, whole-file reads. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

FILE *boot_fopen(const char *path, const char *mode);
int boot_seek(FILE *f, uint64_t offset);
/* Creates the folder and its parents; 0 on success. */
int boot_mkdir_p(const char *path);
int boot_is_file(const char *path);
/* Size and modification time (seconds); 0 on success. */
int boot_stat(const char *path, uint64_t *size, int64_t *mtime);
/* Replaces `to` with `from`; 0 on success. */
int boot_rename(const char *from, const char *to);
int boot_remove(const char *path);
/* The whole file, malloc'd (free it); NULL when it cannot be read. */
uint8_t *boot_read_file(const char *path, size_t *size);
int boot_write_file(const char *path, const void *data, size_t size);
/* Calls fn for each regular file name in `dir` (not recursive); stops when fn returns nonzero. */
int boot_list_dir(const char *dir, int (*fn)(void *ctx, const char *name), void *ctx);
/* snprintf into a fixed buffer; 1 when it fit. */
int boot_path(char *out, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
