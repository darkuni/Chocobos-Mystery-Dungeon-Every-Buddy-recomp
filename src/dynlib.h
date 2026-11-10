/* sceKernelLoadStartModule / sceKernelDlsym for modules linked at prepare time (see dynlib.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* boot_path: boot-linked.bin (modules.txt sits beside it); app0: the game folder;
 * bases: the module table's image offsets, in link order. */
int dynlib_init(const char *boot_path, const char *app0, unsigned char *image, const uint64_t *bases, size_t count);
/* Handle for a linked module matched by base name (path and .prx/.sprx ignored), or -1. */
int32_t dynlib_load(const char *path);
/* Host address of an exported symbol by C name, or 0. */
uintptr_t dynlib_sym(int32_t handle, const char *name);
/* NID of a symbol name (11 characters + NUL). */
void dynlib_nid(const char *name, char out[12]);
