/* First-start preparation of a game folder for the loader: the C version of scripts/prepare.py +
 * link_modules.py + function_starts.py + content_profile.py. Writes, into out_dir:
 *   boot-linked.bin (G3PROBE5: eboot + the game's own modules, linked), functions.bin (BBFUNC01:
 *   function starts for red-zone protection on Windows), content.bin (G3CONT01, SKU full).
 * Byte-for-byte the same files as the Python scripts (tests/test_boot_prepare.sh checks). */
#pragma once
#include <stddef.h>

/* modules: the game's sce_module files to link, in order (NULL-terminated). 0 on success. */
int boot_prepare(const char *game_dir, const char *out_dir, const char *const *modules, char *err, size_t err_size);
