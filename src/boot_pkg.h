/* Unpacks a PS4 fake PKG into a game folder at first start: the C version of
 * scripts/pkg_extract.py (same keys, same files, sce_sys from the PKG's metadata entries). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char content_id[37];
    char title_id[16];
    char category[8];  /* "gd" game, "gp" update, "ac" add-on */
    char app_ver[8];
    char title[128];
} BootPkgInfo;

/* Header and param.sfo of a PKG; 0 on success, else err holds the reason. */
int boot_pkg_info(const char *path, BootPkgInfo *info, char *err, size_t err_size);

typedef void (*BootPkgProgress)(void *ctx, uint64_t done, uint64_t total);
/* Unpacks `path` into `out_dir` (files there with the same names are replaced). progress gets the
 * bytes written so far and the total. 0 on success, else err holds the reason. */
int boot_pkg_extract(const char *path, const char *out_dir, BootPkgProgress progress, void *ctx,
                     char *err, size_t err_size);
/* Total file bytes the unpack writes (for one progress bar over several PKGs); 0 on error. */
uint64_t boot_pkg_payload_size(const char *path);
