/* sceKernelLoadStartModule / sceKernelDlsym for modules linked at prepare time (see dynlib.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* boot_path: boot-linked.bin (modules.txt sits beside it); app0: the game folder;
 * bases: the module table's image offsets, in link order. */
int dynlib_init(const char *boot_path, const char *app0, unsigned char *image, const uint64_t *bases,
                const uint64_t *inits, size_t count);
/* 1 for a module the game opens itself (Unity's Media/Plugins): it is not started with the others
 * but on the first sceKernelLoadStartModule, with the game's arguments. */
int dynlib_deferred(size_t index);
/* Handle for a linked module matched by base name (path and .prx/.sprx ignored), or -1; starts a
 * deferred module on first use (module_start(argc, argv), its result in *result). */
int32_t dynlib_load(const char *path, uint64_t argc, const void *argv, int32_t *result);
/* Host address of an exported symbol by C name, or 0. */
uintptr_t dynlib_sym(int32_t handle, const char *name);
/* NID of a symbol name (11 characters + NUL). */
void dynlib_nid(const char *name, char out[12]);
