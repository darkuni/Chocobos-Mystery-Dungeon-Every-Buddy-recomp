/* See boot_prepare.h. Each function names the Python it follows; checks and errors are kept. */
#include "boot_prepare.h"
#include "boot_util.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT_512M (512ull * 1024 * 1024)
enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_PROCPARAM = 0x61000001,
       PT_SCE_RELRO = 0x61000010, PT_GNU_EH_FRAME = 0x6474e550 };
enum { DT_INIT = 12, DT_JMPREL = 0x61000029, DT_PLTRELSZ = 0x6100002d, DT_RELA = 0x6100002f, DT_RELASZ = 0x61000031,
       DT_STRTAB = 0x61000035, DT_STRSZ = 0x61000037, DT_SYMTAB = 0x61000039, DT_SYMTABSZ = 0x6100003f,
       DT_NEEDED_MODULE = 0x6100000d, DT_EXPORT_MODULE = 0x6100000f, DT_EXPORT_LIB = 0x61000013, DT_IMPORT_LIB = 0x61000015 };

static char *g_err;
static size_t g_err_size;
static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_err, g_err_size, fmt, args);
    va_end(args);
    return -1;
}
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t le64(const uint8_t *b) { return (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32; }
static void put64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t align64k(uint64_t v) { return (v + 65535) & ~(uint64_t)65535; }

/* ---------------------------------------------------------------- growable buffers */
typedef struct { uint8_t *data; size_t size, cap; } Buf;
static int buf_reserve(Buf *b, size_t size) {
    if (size <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < size) cap *= 2;
    uint8_t *grown = realloc(b->data, cap);
    if (!grown) return fail("out of memory");
    b->data = grown; b->cap = cap;
    return 0;
}
static int buf_resize_zero(Buf *b, size_t size) { /* image.extend(bytes(...)) */
    if (size > b->size) {
        if (buf_reserve(b, size)) return -1;
        memset(b->data + b->size, 0, size - b->size);
    }
    b->size = size;
    return 0;
}
static int buf_put(Buf *b, const void *data, size_t size) {
    if (buf_reserve(b, b->size + size)) return -1;
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}
static int buf_q(Buf *b, uint64_t v) { uint8_t x[8]; put64(x, v); return buf_put(b, x, 8); }

/* ---------------------------------------------------------------- prepare.py parse_self */
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Ph;
typedef struct {
    uint8_t *elf; size_t elf_size;
    uint64_t entry;
    Ph ph[256]; int phnum;
} Self;
static int parse_self(const uint8_t *data, size_t size, Self *out, const char *file) {
    memset(out, 0, sizeof(*out));
    if (size < 64) return fail("%s: empty or truncated SELF", file);
    int plain = !memcmp(data, "\x7f" "ELF", 4);
    size_t count = 0, base = 0;
    if (!plain) {
        if (memcmp(data, "O\x15=\x1d", 4)) return fail("%s: expected PS4 SELF or ELF", file);
        count = le16(data + 24);
        base = 32 + count * 32;
    }
    if (base + 64 > size) return fail("%s: truncated SELF", file);
    const uint8_t *h = data + base;
    if (memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) || le16(h + 18) != 62) return fail("%s: expected little-endian x86-64 ELF", file);
    uint64_t phoff = le64(h + 32);
    unsigned phentsize = le16(h + 54), phnum = le16(h + 56);
    if (phentsize != 56 || !(0 < phnum && phnum < 256)) return fail("%s: unsupported program headers", file);
    out->entry = le64(h + 24);
    out->phnum = (int)phnum;
    if (base + phoff + (uint64_t)phnum * 56 > size) return fail("%s: truncated program headers", file);
    uint64_t header_end = phoff + (uint64_t)phnum * 56, end = header_end;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *r = h + phoff + i * 56;
        Ph *p = &out->ph[i];
        p->type = le32(r); p->flags = le32(r + 4); p->offset = le64(r + 8); p->vaddr = le64(r + 16);
        p->paddr = le64(r + 24); p->filesz = le64(r + 32); p->memsz = le64(r + 40); p->align = le64(r + 48);
        if (p->offset + p->filesz > end) end = p->offset + p->filesz;
    }
    if (end > LIMIT_512M) return fail("%s: probe image exceeds 512 MiB limit", file);
    if (plain) {
        if (end > size) return fail("%s: truncated ELF", file);
        out->elf = malloc((size_t)end);
        if (!out->elf) return fail("out of memory");
        memcpy(out->elf, data, (size_t)end);
        out->elf_size = (size_t)end;
        return 0;
    }
    out->elf = calloc(1, (size_t)end);
    if (!out->elf) return fail("out of memory");
    out->elf_size = (size_t)end;
    memcpy(out->elf, h, (size_t)header_end);
    uint64_t covered[257][2];
    size_t ncovered = 0;
    covered[ncovered][0] = 0; covered[ncovered++][1] = header_end;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *s = data + 32 + i * 32;
        uint64_t flags = le64(s), off = le64(s + 8), sz = le64(s + 16), mem = le64(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) return fail("%s: encrypted/compressed SELF segment is unsupported", file);
        unsigned index = (unsigned)(flags >> 20) & 4095;
        if (index >= phnum) return fail("%s: invalid segment index", file);
        const Ph *p = &out->ph[index];
        if (sz != p->filesz || sz != mem) return fail("%s: unsupported blocked segment layout", file);
        if (off + sz > size) return fail("%s: truncated SELF segment", file);
        memcpy(out->elf + p->offset, data + off, (size_t)sz);
        if (ncovered < 257) { covered[ncovered][0] = p->offset; covered[ncovered++][1] = p->offset + sz; }
    }
    for (unsigned i = 0; i < phnum; ++i) {
        const Ph *p = &out->ph[i];
        if (!p->filesz) continue;
        int ok = 0;
        for (size_t c = 0; c < ncovered && !ok; ++c) ok = covered[c][0] <= p->offset && p->offset + p->filesz <= covered[c][1];
        if (!ok && (p->type == PT_LOAD || p->type == PT_DYNAMIC || p->type == PT_SCE_DYNLIBDATA || p->type == PT_SCE_RELRO))
            return fail("%s: required segment %u is unavailable", file, i);
    }
    return 0;
}
static const Ph *find_ph(const Self *s, uint32_t type) {
    for (int i = 0; i < s->phnum; ++i)
        if (s->ph[i].type == type) return &s->ph[i];
    return NULL;
}

/* ---------------------------------------------------------------- link_modules.py module() */
typedef struct { char key[12]; const char *name; uint16_t version; } Lib;
typedef struct {
    char nid[16];
    const char *lib; uint16_t lib_version;
    const char *mod; uint16_t mod_version;
    int valid;
} Ident;
typedef struct {
    const char *name;
    uint32_t type, binding, section;
    uint64_t value, size;
    Ident identity;
} Sym;
typedef struct { uint64_t target; uint32_t kind, sym; int64_t addend; } Rel;
typedef struct {
    Self self;
    const uint8_t *blob; size_t blob_size;
    uint64_t (*dyn)[2]; size_t ndyn;
    const char *strings; size_t strings_size;
    Lib libs[256]; size_t nlibs;
    Lib mods[256]; size_t nmods;
    Sym *syms; size_t nsyms;
    Rel *rels; size_t nrels;
    const char *file;
} Module;

static void encode_id(uint64_t value, char out[12]) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    char tmp[12];
    int n = 0;
    tmp[n++] = alphabet[value & 63];
    value >>= 6;
    while (value) { tmp[n++] = alphabet[value & 63]; value >>= 6; }
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}
static int tag(const Module *m, uint64_t t, uint64_t *value) { /* dict(dynamic): the last one wins */
    int found = 0;
    for (size_t i = 0; i < m->ndyn; ++i)
        if (m->dyn[i][0] == t) { *value = m->dyn[i][1]; found = 1; }
    return found;
}
static int string_at(const Module *m, uint64_t offset, const char **out) {
    if (offset >= m->strings_size || !memchr(m->strings + offset, 0, m->strings_size - (size_t)offset))
        return fail("%s: bad string offset", m->file);
    *out = m->strings + offset;
    return 0;
}
static int blob_span(const Module *m, uint64_t offset_tag, uint64_t size_tag, const uint8_t **out, size_t *size) {
    uint64_t off, sz;
    if (!tag(m, offset_tag, &off) || !tag(m, size_tag, &sz)) return fail("%s: missing dynamic tag %#" PRIx64, m->file, offset_tag);
    if (off + sz > m->blob_size) return fail("%s: out of bounds dynamic table", m->file);
    *out = m->blob + off; *size = (size_t)sz;
    return 0;
}
static int lookup_lib(const Lib *table, size_t n, const char *key, size_t key_len, const Lib **out) {
    for (size_t i = 0; i < n; ++i)
        if (strlen(table[i].key) == key_len && !memcmp(table[i].key, key, key_len)) { *out = &table[i]; return 0; }
    return -1;
}
/* identity(name): (NID, library, module) for NID#lib#mod names; invalid for plain names. */
static int identity_of(const Module *m, const char *name, Ident *out) {
    memset(out, 0, sizeof(*out));
    const char *a = strchr(name, '#'), *b = a ? strchr(a + 1, '#') : NULL;
    if (!a || !b || strchr(b + 1, '#')) return 0;
    if ((size_t)(a - name) >= sizeof(out->nid)) return fail("%s: NID too long", m->file);
    const Lib *lib, *mod;
    if (lookup_lib(m->libs, m->nlibs, a + 1, (size_t)(b - a - 1), &lib) || lookup_lib(m->mods, m->nmods, b + 1, strlen(b + 1), &mod))
        return fail("%s: unknown library or module ID in %s", m->file, name);
    memcpy(out->nid, name, (size_t)(a - name));
    out->lib = lib->name; out->lib_version = lib->version;
    out->mod = mod->name; out->mod_version = mod->version;
    out->valid = 1;
    return 0;
}
static int same_ident(const Ident *x, const Ident *y) {
    if (!x->valid || !y->valid) return x->valid == y->valid;
    return !strcmp(x->nid, y->nid) && !strcmp(x->lib, y->lib) && x->lib_version == y->lib_version
        && !strcmp(x->mod, y->mod) && x->mod_version == y->mod_version;
}
static void free_module(Module *m) {
    free(m->self.elf); free(m->dyn); free(m->syms); free(m->rels);
}
static int load_module(const char *path, const char *file, Module *m, uint8_t **source_out, size_t *source_size) {
    memset(m, 0, sizeof(*m));
    m->file = file;
    size_t size = 0;
    uint8_t *source = boot_read_file(path, &size);
    if (!source) return fail("cannot read %s", path);
    if (!size) { free(source); return fail("%s is empty (0 bytes): the game files are incomplete", file); }
    int r = parse_self(source, size, &m->self, file);
    if (source_out) { *source_out = source; *source_size = size; } else free(source);
    if (r) return -1;
    const Ph *dp = find_ph(&m->self, PT_DYNAMIC), *lp = find_ph(&m->self, PT_SCE_DYNLIBDATA);
    if (!dp || !lp) return fail("%s: no dynamic segment", file);
    if (dp->offset + dp->filesz > m->self.elf_size || lp->offset + lp->filesz > m->self.elf_size) return fail("%s: out of bounds", file);
    m->dyn = malloc((size_t)(dp->filesz / 16 + 1) * sizeof(*m->dyn));
    if (!m->dyn) return fail("out of memory");
    for (uint64_t pos = dp->offset; pos + 16 <= dp->offset + dp->filesz; pos += 16) {
        uint64_t t = le64(m->self.elf + pos), v = le64(m->self.elf + pos + 8);
        if (!t) break;
        m->dyn[m->ndyn][0] = t; m->dyn[m->ndyn++][1] = v;
    }
    m->blob = m->self.elf + lp->offset;
    m->blob_size = (size_t)lp->filesz;
    const uint8_t *strings;
    if (blob_span(m, DT_STRTAB, DT_STRSZ, &strings, &m->strings_size)) return -1;
    m->strings = (const char *)strings;
    for (size_t i = 0; i < m->ndyn; ++i) {
        uint64_t t = m->dyn[i][0], v = m->dyn[i][1];
        if (t != DT_EXPORT_LIB && t != DT_IMPORT_LIB && t != DT_NEEDED_MODULE && t != DT_EXPORT_MODULE) continue;
        int is_lib = t == DT_EXPORT_LIB || t == DT_IMPORT_LIB;
        Lib *table = is_lib ? m->libs : m->mods;
        size_t *n = is_lib ? &m->nlibs : &m->nmods;
        Lib entry;
        encode_id(v >> 48, entry.key);
        if (string_at(m, v & 0xffffffff, &entry.name)) return -1;
        entry.version = (uint16_t)((v >> 32) & 65535);
        const Lib *existing;
        if (!lookup_lib(table, *n, entry.key, strlen(entry.key), &existing)) {
            if (strcmp(existing->name, entry.name) || existing->version != entry.version) return fail("%s: conflicting module/library IDs", file);
            continue;
        }
        if (*n == 256) return fail("%s: too many libraries", file);
        table[(*n)++] = entry;
    }
    const uint8_t *syms;
    size_t syms_size;
    if (blob_span(m, DT_SYMTAB, DT_SYMTABSZ, &syms, &syms_size)) return -1;
    m->nsyms = syms_size / 24;
    m->syms = calloc(m->nsyms ? m->nsyms : 1, sizeof(Sym));
    if (!m->syms) return fail("out of memory");
    for (size_t i = 0; i < m->nsyms; ++i) {
        const uint8_t *s = syms + i * 24;
        Sym *y = &m->syms[i];
        if (string_at(m, le32(s), &y->name)) return -1;
        y->type = s[4] & 15; y->binding = s[4] >> 4; y->section = le16(s + 6);
        y->value = le64(s + 8); y->size = le64(s + 16);
        if (*y->name && identity_of(m, y->name, &y->identity)) return -1;
    }
    const uint64_t tables[2][2] = {{DT_JMPREL, DT_PLTRELSZ}, {DT_RELA, DT_RELASZ}};
    size_t total = 0;
    for (int t = 0; t < 2; ++t) {
        const uint8_t *table;
        size_t sz;
        if (blob_span(m, tables[t][0], tables[t][1], &table, &sz)) return -1;
        total += sz / 24;
    }
    m->rels = malloc((total ? total : 1) * sizeof(Rel));
    if (!m->rels) return fail("out of memory");
    for (int t = 0; t < 2; ++t) {
        const uint8_t *table;
        size_t sz;
        blob_span(m, tables[t][0], tables[t][1], &table, &sz);
        if (sz % 24) return fail("%s: bad relocation table size", file);
        for (size_t pos = 0; pos + 24 <= sz; pos += 24) {
            Rel *r = &m->rels[m->nrels++];
            uint64_t info = le64(table + pos + 8);
            r->target = le64(table + pos); r->kind = (uint32_t)info; r->sym = (uint32_t)(info >> 32);
            r->addend = (int64_t)le64(table + pos + 16);
        }
    }
    return 0;
}
static int sym_index(const Module *m, uint32_t index, const Sym **out) {
    if (index >= m->nsyms) return fail("%s: bad symbol index", m->file);
    *out = &m->syms[index];
    return 0;
}

/* ---------------------------------------------------------------- prepare.py inspect_libc */
static int libc_init_env_is_ret(const char *path, int *is_ret) {
    *is_ret = 0;
    Module m;
    if (load_module(path, "libc.prx", &m, NULL, NULL)) { free_module(&m); return -1; }
    for (size_t i = 0; i < m.nsyms; ++i) {
        const Sym *s = &m.syms[i];
        if (!s->section || s->type != 2) continue;
        size_t len = strcspn(s->name, "#");
        int init_env = len == 11 && !memcmp(s->name, "bzQExy189ZI", 11);        /* nid('_init_env') */
        int ios_init = len == 11 && !memcmp(s->name, "sqWytnhYdEg", 11);        /* nid('_ZNSt8ios_base4InitC1Ev') */
        if (!init_env && !ios_init) continue;
        const Ph *p = NULL;
        for (int j = 0; j < m.self.phnum && !p; ++j) {
            const Ph *q = &m.self.ph[j];
            if (q->type == PT_LOAD && (q->flags & 1) && q->vaddr <= s->value && s->value < q->vaddr + q->filesz) p = q;
        }
        if (!p) { free_module(&m); return fail("libc.prx: symbol outside its code"); }
        uint64_t at = p->offset + s->value - p->vaddr;
        if (at + s->size > m.self.elf_size) { free_module(&m); return fail("libc.prx: out of bounds"); }
        if (init_env) *is_ret = s->size == 1 && m.self.elf[at] == 0xc3;
    }
    free_module(&m);
    return 0;
}

/* ---------------------------------------------------------------- prepare.py prepare() */
typedef struct { uint64_t target, kind; int64_t value, addend; } Out;  /* '<QQqq' */
typedef struct { uint64_t vaddr, memsz, flags; } Seg;
typedef struct {
    Buf image;
    uint64_t size, entry, flags;
    Seg *segs; size_t nsegs, segcap;
    char (*names)[128]; size_t nnames, namecap;
    Out *relocs; size_t nrelocs, relcap;
} Probe;
static int add_seg(Probe *p, uint64_t vaddr, uint64_t memsz, uint64_t flags) {
    if (p->nsegs == p->segcap) {
        p->segcap = p->segcap ? p->segcap * 2 : 16;
        Seg *g = realloc(p->segs, p->segcap * sizeof(Seg));
        if (!g) return fail("out of memory");
        p->segs = g;
    }
    p->segs[p->nsegs++] = (Seg){vaddr, memsz, flags};
    return 0;
}
static int add_reloc(Probe *p, uint64_t target, uint64_t kind, int64_t value, int64_t addend) {
    if (p->nrelocs == p->relcap) {
        p->relcap = p->relcap ? p->relcap * 2 : 4096;
        Out *g = realloc(p->relocs, p->relcap * sizeof(Out));
        if (!g) return fail("out of memory");
        p->relocs = g;
    }
    p->relocs[p->nrelocs++] = (Out){target, kind, value, addend};
    return 0;
}
static int add_name(Probe *p, const char *name) {
    if (strlen(name) >= 128) return fail("import name too long");
    if (p->nnames == p->namecap) {
        p->namecap = p->namecap ? p->namecap * 2 : 256;
        char (*g)[128] = realloc(p->names, p->namecap * 128);
        if (!g) return fail("out of memory");
        p->names = g;
    }
    memset(p->names[p->nnames], 0, 128);
    strcpy(p->names[p->nnames++], name);
    return 0;
}
static int load_mapped(const Ph *const *loads, size_t n, uint64_t addr, uint64_t size) {
    for (size_t i = 0; i < n; ++i)
        if (loads[i]->vaddr <= addr && addr + size <= loads[i]->vaddr + loads[i]->memsz) return 1;
    return 0;
}

static int prepare_eboot(const Module *main, const char *game, Probe *p) {
    const Ph *loads[256];
    size_t nloads = 0;
    for (int i = 0; i < main->self.phnum; ++i)
        if (main->self.ph[i].type == PT_LOAD || main->self.ph[i].type == PT_SCE_RELRO) loads[nloads++] = &main->self.ph[i];
    uint64_t size = 0;
    for (size_t i = 0; i < nloads; ++i)
        if (loads[i]->vaddr + loads[i]->memsz > size) size = loads[i]->vaddr + loads[i]->memsz;
    if (size > LIMIT_512M || !load_mapped(loads, nloads, main->self.entry, 1)) return fail("invalid memory image");
    if (buf_resize_zero(&p->image, (size_t)size)) return -1;
    for (size_t i = 0; i < nloads; ++i) {
        const Ph *l = loads[i];
        if (l->filesz > l->memsz) return fail("segment file size exceeds memory size");
        memcpy(p->image.data + l->vaddr, main->self.elf + l->offset, (size_t)l->filesz);
    }
    p->size = size;
    p->entry = main->self.entry;
    uint32_t *import_index = malloc((main->nsyms ? main->nsyms : 1) * sizeof(uint32_t));
    if (!import_index) return fail("out of memory");
    memset(import_index, 0xff, (main->nsyms ? main->nsyms : 1) * sizeof(uint32_t));
    int r = 0;
    for (size_t i = 0; i < main->nrels && !r; ++i) {
        const Rel *rel = &main->rels[i];
        if (!load_mapped(loads, nloads, rel->target, 8)) { r = fail("relocation outside mapped segments: %#" PRIx64, rel->target); break; }
        if (rel->kind == 8) { r = add_reloc(p, rel->target, 0, rel->addend, 0); continue; }
        if (rel->kind != 1 && rel->kind != 6 && rel->kind != 7) { r = fail("unsupported relocation %u", rel->kind); break; }
        const Sym *s = NULL;
        if ((r = sym_index(main, rel->sym, &s))) break;
        if (s->section) {
            if (s->section == 0xfff1) put64(p->image.data + rel->target, s->value + (uint64_t)rel->addend);
            else r = add_reloc(p, rel->target, 0, (int64_t)(s->value + (uint64_t)rel->addend), 0);
            continue;
        }
        if (import_index[rel->sym] == 0xffffffffu) {
            import_index[rel->sym] = (uint32_t)p->nnames;
            if ((r = add_name(p, s->name))) break;
        }
        uint64_t kind = s->type == 1 ? 2 : 1;
        if ((kind == 1 && rel->addend) || !(0 <= rel->addend && rel->addend < 4096)) { r = fail("unsupported import addend"); break; }
        r = add_reloc(p, rel->target, kind, import_index[rel->sym], rel->addend);
    }
    free(import_index);
    if (r) return -1;
    for (size_t i = 0; i < nloads; ++i)
        if (add_seg(p, loads[i]->vaddr, loads[i]->memsz, loads[i]->flags)) return -1;
    char libc[4096];
    int is_ret = 0;
    if (!boot_path(libc, sizeof(libc), "%s/sce_module/libc.prx", game) || libc_init_env_is_ret(libc, &is_ret)) return -1;
    p->flags = (uint64_t)is_ret;
    return 0;
}

/* ---------------------------------------------------------------- link_modules.py link() */
typedef struct { Ident id; uint64_t address, kind; } Export;
typedef struct { Ident id; size_t index; } Identity;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } Table;
static const uint8_t FS_LOAD[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}; /* mov rax, fs:[0] */
static int patch_fs_loads(Buf *image, const Self *s, uint64_t base) {
    int patched = 0;
    for (int i = 0; i < s->phnum; ++i) {
        const Ph *p = &s->ph[i];
        if (p->type != PT_LOAD || !(p->flags & 1)) continue;
        uint64_t start = base + p->vaddr, end = base + p->vaddr + p->filesz;
        if (end > image->size) end = image->size;
        for (uint64_t at = start; at + sizeof(FS_LOAD) <= end;) {
            if (!memcmp(image->data + at, FS_LOAD, sizeof(FS_LOAD))) { image->data[at] = 0x65; ++patched; at += sizeof(FS_LOAD); }
            else ++at;
        }
    }
    return patched;
}
static void canonical(const Ident *id, const char *fallback, char *out) {
    if (id->valid) snprintf(out, 128, "%s#%s", id->nid, id->lib);
    else snprintf(out, 128, "%s", fallback);
}

typedef struct {
    Identity *ids; size_t nids, idcap;
    Export *exports; size_t nexports, excap;
} LinkState;
static int find_identity(const LinkState *l, const Ident *id) {
    for (size_t i = 0; i < l->nids; ++i)
        if (same_ident(&l->ids[i].id, id)) return (int)i;
    return -1;
}
static int add_identity(LinkState *l, const Ident *id, size_t index) {
    int at = find_identity(l, id);
    if (at >= 0) { l->ids[at].index = index; return 0; }   /* dict: a later key keeps its place */
    if (l->nids == l->idcap) {
        l->idcap = l->idcap ? l->idcap * 2 : 1024;
        Identity *g = realloc(l->ids, l->idcap * sizeof(Identity));
        if (!g) return fail("out of memory");
        l->ids = g;
    }
    l->ids[l->nids++] = (Identity){*id, index};
    return 0;
}
static int imported(LinkState *l, Probe *p, const Sym *s, uint64_t *index) {
    int at = find_identity(l, &s->identity);
    if (at < 0) {
        char name[128];
        canonical(&s->identity, "", name);
        if (add_identity(l, &s->identity, p->nnames) || add_name(p, name)) return -1;
        at = (int)l->nids - 1;
    }
    *index = l->ids[at].index;
    return 0;
}

static int link_modules(const Module *main, const char *game, const char *const *modules, Probe *p, const char *out_dir,
                        Table *table, size_t *ntable, uint64_t *bases) {
    LinkState l = {0};
    int r = 0;
    for (size_t i = 0; i < p->nnames && !r; ++i) {
        Ident id;
        if ((r = identity_of(main, p->names[i], &id))) break;
        r = add_identity(&l, &id, i);
        char name[128];
        canonical(&id, p->names[i], name);
        strcpy(p->names[i], name);
    }
    uint64_t base = align64k(p->size);
    int fs_patched = r ? 0 : patch_fs_loads(&p->image, &main->self, 0);
    uint64_t tls_module = 2;
    Module *mods = calloc(16, sizeof(Module));
    size_t nmods = 0;
    if (!mods) r = fail("out of memory");
    for (size_t k = 0; !r && modules[k]; ++k) {
        if (nmods == 16) { r = fail("too many modules"); break; }
        Module *m = &mods[nmods++];
        char path[4096];
        if (!boot_path(path, sizeof(path), "%s/sce_module/%s", game, modules[k])) { r = fail("path too long"); break; }
        if ((r = load_module(path, modules[k], m, NULL, NULL))) break;
        const Ph *loads[256];
        size_t nloads = 0;
        for (int i = 0; i < m->self.phnum; ++i)
            if (m->self.ph[i].type == PT_LOAD || m->self.ph[i].type == PT_SCE_RELRO) loads[nloads++] = &m->self.ph[i];
        uint64_t modsize = 0;
        for (size_t i = 0; i < nloads; ++i)
            if (loads[i]->vaddr + loads[i]->memsz > modsize) modsize = loads[i]->vaddr + loads[i]->memsz;
        if (base + modsize > LIMIT_512M) { r = fail("linked image exceeds probe limit"); break; }
        if ((r = buf_resize_zero(&p->image, (size_t)(base + modsize)))) break;
        for (size_t i = 0; i < nloads && !r; ++i) {
            if (loads[i]->filesz > loads[i]->memsz) { r = fail("%s: invalid segment size", m->file); break; }
            memcpy(p->image.data + base + loads[i]->vaddr, m->self.elf + loads[i]->offset, (size_t)loads[i]->filesz);
            r = add_seg(p, base + loads[i]->vaddr, loads[i]->memsz, loads[i]->flags);
        }
        if (r) break;
        const Ph *tls = find_ph(&m->self, PT_TLS);
        uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !load_mapped(loads, nloads, tls->vaddr, tls->memsz)) { r = fail("%s: unsupported TLS layout", m->file); break; }
            module_id = tls_module++;
        }
        for (size_t i = 0; i < m->nrels && !r; ++i) {
            const Rel *rel = &m->rels[i];
            if (!load_mapped(loads, nloads, rel->target, 8)) { r = fail("%s: relocation outside image %#" PRIx64, m->file, rel->target); break; }
            uint64_t target = rel->target + base;
            if (rel->kind == 8) { r = add_reloc(p, target, 0, (int64_t)(base + (uint64_t)rel->addend), 0); continue; }
            const Sym *s = NULL;
            if (rel->kind == 16) {
                if ((r = sym_index(m, rel->sym, &s))) break;
                if (*s->name || s->type != 3 || rel->addend || !module_id) { r = fail("%s: unsupported external TLS relocation", m->file); break; }
                put64(p->image.data + target, module_id);
                continue;
            }
            if (rel->kind != 1 && rel->kind != 6 && rel->kind != 7) { r = fail("%s: unsupported relocation type %u", m->file, rel->kind); break; }
            if ((r = sym_index(m, rel->sym, &s))) break;
            if (s->section) {
                if (s->section == 0xfff1) put64(p->image.data + target, s->value + (uint64_t)rel->addend);
                else r = add_reloc(p, target, 0, (int64_t)(base + s->value + (uint64_t)rel->addend), 0);
            } else if (!s->identity.valid && s->binding == 2) {
                put64(p->image.data + target, 0);  /* weak undefined (module_start/stop) */
            } else {
                uint64_t rk = s->type == 1 ? 2 : 1, index;
                if ((rk == 1 && rel->addend) || !(0 <= rel->addend && rel->addend < 4096)) { r = fail("%s: unsupported import addend", m->file); break; }
                if ((r = imported(&l, p, s, &index))) break;
                r = add_reloc(p, target, rk, (int64_t)index, rel->addend);
            }
        }
        if (r) break;
        for (size_t i = 0; i < m->nsyms && !r; ++i) {
            const Sym *s = &m->syms[i];
            if (!s->section || (s->binding != 1 && s->binding != 2) || !s->identity.valid || (s->type != 1 && s->type != 2)) continue;
            if (!load_mapped(loads, nloads, s->value, s->size ? s->size : 1)) { r = fail("%s: export outside load segments", m->file); break; }
            for (size_t e = 0; e < l.nexports; ++e)
                if (same_ident(&l.exports[e].id, &s->identity)) { r = fail("%s: ambiguous export", m->file); break; }
            if (r) break;
            if (l.nexports == l.excap) {
                l.excap = l.excap ? l.excap * 2 : 4096;
                Export *g = realloc(l.exports, l.excap * sizeof(Export));
                if (!g) { r = fail("out of memory"); break; }
                l.exports = g;
            }
            l.exports[l.nexports++] = (Export){s->identity, base + s->value, s->type == 1 ? 2u : 1u};
        }
        if (r) break;
        fs_patched += patch_fs_loads(&p->image, &m->self, base);
        uint64_t init = 0;
        tag(m, DT_INIT, &init);
        table[*ntable] = (Table){base, modsize, base + init, tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0,
                                 tls ? tls->filesz : 0, module_id};
        bases[(*ntable)++] = base;
        base = align64k(base + modsize);
    }
    /* Bindings: each import identity to a native export (libSceLibcInternal falls back to libc's). */
    Buf bindings = {0};
    size_t nbindings = 0, unresolved = 0;
    for (size_t i = 0; !r && i < l.nids; ++i) {
        const Ident *id = &l.ids[i].id;
        const Export *found = NULL;
        for (size_t e = 0; e < l.nexports && !found; ++e)
            if (same_ident(&l.exports[e].id, id)) found = &l.exports[e];
        if (!found && id->valid && !strcmp(id->lib, "libSceLibcInternal")) {
            size_t matches = 0;
            const Export *candidate = NULL;
            for (size_t e = 0; e < l.nexports; ++e)
                if (!strcmp(l.exports[e].id.nid, id->nid) && !strcmp(l.exports[e].id.lib, "libc")) { ++matches; candidate = &l.exports[e]; }
            if (matches == 1) found = candidate;
        }
        if (found) {
            r = buf_q(&bindings, l.ids[i].index) || buf_q(&bindings, found->address) || buf_q(&bindings, found->kind);
            ++nbindings;
        } else ++unresolved;
    }
    const Ph *main_tls = find_ph(&main->self, PT_TLS), *procparam = find_ph(&main->self, PT_SCE_PROCPARAM);
    uint64_t tls_values[4] = {0};
    if (!r && main_tls) {
        tls_values[0] = main_tls->vaddr; tls_values[1] = main_tls->filesz; tls_values[2] = main_tls->memsz; tls_values[3] = main_tls->align;
        if (main_tls->filesz > main_tls->memsz || main_tls->memsz > 1024 * 1024 || main_tls->vaddr + main_tls->filesz > p->size)
            r = fail("unsupported eboot TLS layout");
    }
    if (!r && !procparam) r = fail("eboot.bin without process parameters");
    if (!r) {
        Buf f = {0};
        static const char magic[] = "G3PROBE5";
        r = buf_put(&f, magic, 8) || buf_q(&f, p->image.size) || buf_q(&f, p->entry) || buf_q(&f, p->nsegs)
            || buf_q(&f, p->nrelocs) || buf_q(&f, p->nnames) || buf_q(&f, p->flags) || buf_q(&f, procparam->vaddr);
        for (int i = 0; !r && i < 4; ++i) r = buf_q(&f, tls_values[i]);
        r = r || buf_q(&f, *ntable);
        for (size_t i = 0; !r && i < *ntable; ++i)
            r = buf_q(&f, table[i].base) || buf_q(&f, table[i].size) || buf_q(&f, table[i].init) || buf_q(&f, table[i].tls_address)
                || buf_q(&f, table[i].tls_memsz) || buf_q(&f, table[i].tls_filesz) || buf_q(&f, table[i].tls_module);
        r = r || buf_q(&f, nbindings) || buf_put(&f, bindings.data, bindings.size);
        for (size_t i = 0; !r && i < p->nsegs; ++i) r = buf_q(&f, p->segs[i].vaddr) || buf_q(&f, p->segs[i].memsz) || buf_q(&f, p->segs[i].flags);
        r = r || buf_put(&f, p->names, p->nnames * 128);
        for (size_t i = 0; !r && i < p->nrelocs; ++i)
            r = buf_q(&f, p->relocs[i].target) || buf_q(&f, p->relocs[i].kind) || buf_q(&f, (uint64_t)p->relocs[i].value)
                || buf_q(&f, (uint64_t)p->relocs[i].addend);
        r = r || buf_put(&f, p->image.data, p->image.size);
        char path[4096], tmp[4200];
        if (!r && (!boot_path(path, sizeof(path), "%s/boot-linked.bin", out_dir) || !boot_path(tmp, sizeof(tmp), "%s.part", path)
                   || boot_write_file(tmp, f.data, f.size) || boot_rename(tmp, path)))
            r = fail("cannot write %s/boot-linked.bin", out_dir);
        free(f.data);
        if (!r) {
            printf("Linked modules: ");
            for (size_t i = 0; i < *ntable; ++i) printf("%s%s@%#" PRIx64, i ? ", " : "", modules[i], table[i].base);
            printf("; %zu native bindings, %zu imports left to the host runtime, fs->gs patched=%d\n", nbindings, unresolved, fs_patched);
        }
    }
    free(bindings.data);
    for (size_t i = 0; i < nmods; ++i) free_module(&mods[i]);
    free(mods);
    free(l.ids); free(l.exports);
    return r;
}

/* ---------------------------------------------------------------- function_starts.py */
static int read_encoded(const uint8_t *data, size_t size, size_t *pos, unsigned encoding, uint64_t hdr_address, int64_t *value) {
    unsigned form = encoding & 0x0f, application = encoding & 0x70;
    size_t width;
    if (form == 0x03 || form == 0x0b) width = 4;
    else if (form == 0x04 || form == 0x0c) width = 8;
    else return fail("unsupported .eh_frame_hdr encoding %#x", encoding);
    if (*pos + width > size) return fail("truncated .eh_frame_hdr");
    int64_t v = width == 4 ? (form == 0x0b ? (int64_t)(int32_t)le32(data + *pos) : (int64_t)le32(data + *pos))
                           : (int64_t)le64(data + *pos);
    *pos += width;
    if (application == 0x10) v += (int64_t)(hdr_address + *pos - width);
    else if (application == 0x30) v += (int64_t)hdr_address;
    else if (application) return fail("unsupported .eh_frame_hdr application %#x", encoding);
    *value = v;
    return 0;
}
typedef struct { uint64_t *v; size_t n, cap; } Starts;
static int function_starts(const Self *s, uint64_t base, Starts *out) {
    const Ph *hdr = find_ph(s, PT_GNU_EH_FRAME);
    if (!hdr) return 0;
    if (hdr->offset + hdr->filesz > s->elf_size || hdr->filesz < 4) return fail("bad .eh_frame_hdr");
    const uint8_t *data = s->elf + hdr->offset;
    size_t size = (size_t)hdr->filesz, pos = 4;
    if (data[0] != 1 || data[3] == 0xff) return fail("no .eh_frame_hdr search table");
    int64_t ignored, count;
    if (read_encoded(data, size, &pos, data[1], hdr->vaddr, &ignored) || read_encoded(data, size, &pos, data[2], hdr->vaddr, &count)) return -1;
    for (int64_t i = 0; i < count; ++i) {
        int64_t start;
        if (read_encoded(data, size, &pos, data[3], hdr->vaddr, &start) || read_encoded(data, size, &pos, data[3], hdr->vaddr, &ignored)) return -1;
        for (int j = 0; j < s->phnum; ++j) {
            const Ph *p = &s->ph[j];
            if (p->type != PT_LOAD || !(p->flags & 1) || start < (int64_t)p->vaddr || start >= (int64_t)(p->vaddr + p->memsz)) continue;
            if (out->n == out->cap) {
                out->cap = out->cap ? out->cap * 2 : 65536;
                uint64_t *g = realloc(out->v, out->cap * sizeof(uint64_t));
                if (!g) return fail("out of memory");
                out->v = g;
            }
            out->v[out->n++] = base + (uint64_t)start;
            break;
        }
    }
    return 0;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int write_function_starts(const Self *main, const char *game, const char *const *modules, const uint64_t *bases,
                                 size_t nbases, const char *out_dir) {
    Starts st = {0};
    int r = function_starts(main, 0, &st);
    for (size_t k = 0; !r && k < nbases; ++k) {
        char path[4096];
        Module m;
        if (!boot_path(path, sizeof(path), "%s/sce_module/%s", game, modules[k])) { r = fail("path too long"); break; }
        size_t size;
        uint8_t *data = boot_read_file(path, &size);
        if (!data) { r = fail("cannot read %s", path); break; }
        memset(&m, 0, sizeof(m));
        r = parse_self(data, size, &m.self, modules[k]) || function_starts(&m.self, bases[k], &st);
        free(data);
        free(m.self.elf);
    }
    if (!r) {
        qsort(st.v, st.n, sizeof(uint64_t), cmp_u64);
        size_t n = 0;
        for (size_t i = 0; i < st.n; ++i)
            if (!n || st.v[i] != st.v[n - 1]) st.v[n++] = st.v[i];
        Buf f = {0};
        r = buf_put(&f, "BBFUNC01", 8) || buf_q(&f, n);
        for (size_t i = 0; !r && i < n; ++i) r = buf_q(&f, st.v[i]);
        char path[4096];
        if (!r && (!boot_path(path, sizeof(path), "%s/functions.bin", out_dir) || boot_write_file(path, f.data, f.size)))
            r = fail("cannot write functions.bin");
        free(f.data);
        if (!r) printf("Functions: %zu starts for red-zone protection\n", n);
    }
    free(st.v);
    return r;
}

/* ---------------------------------------------------------------- content_profile.py */
static int write_content_profile(const char *game, const char *out_dir) {
    char path[4096];
    size_t size;
    if (!boot_path(path, sizeof(path), "%s/sce_sys/param.sfo", game)) return fail("path too long");
    uint8_t *sfo = boot_read_file(path, &size);
    if (!sfo || size < 20 || memcmp(sfo, "\0PSF", 4)) { free(sfo); return fail("cannot read %s", path); }
    uint32_t params[4] = {0};
    uint32_t keys = le32(sfo + 8), values = le32(sfo + 12), count = le32(sfo + 16);
    int r = 0;
    for (uint32_t i = 0; i < count && 20 + (size_t)(i + 1) * 16 <= size && !r; ++i) {
        const uint8_t *e = sfo + 20 + i * 16;
        uint32_t koff = le16(e), fmt = le16(e + 2), len = le32(e + 4), doff = le32(e + 12);
        if (keys + koff >= size) continue;
        const char *key = (const char *)sfo + keys + koff;
        if (strncmp(key, "USER_DEFINED_PARAM_", 19) || key[19] < '1' || key[19] > '4' || key[20]) continue;
        if (fmt != 0x404 || len != 4 || values + doff + 4 > size) { r = fail("invalid user-defined parameter"); break; }
        params[key[19] - '1'] = le32(sfo + values + doff);
    }
    free(sfo);
    if (r) return -1;
    uint8_t out[28];
    memcpy(out, "G3CONT01", 8);
    uint32_t v[5] = {3, params[0], params[1], params[2], params[3]};  /* SKU full */
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[8 + i * 4 + j] = (uint8_t)(v[i] >> (8 * j));
    if (!boot_path(path, sizeof(path), "%s/content.bin", out_dir) || boot_write_file(path, out, sizeof(out)))
        return fail("cannot write content.bin");
    printf("AppContent profile: SKU=full, user params=[%u, %u, %u, %u], mounted add-ons=0\n", params[0], params[1], params[2], params[3]);
    return 0;
}

int boot_prepare(const char *game, const char *out_dir, const char *const *modules, char *err, size_t err_size) {
    g_err = err; g_err_size = err_size;
    err[0] = 0;
    if (boot_mkdir_p(out_dir)) return fail("cannot create %s", out_dir);
    char path[4096];
    Module main;
    Probe p = {0};
    Table table[16];
    uint64_t bases[16];
    size_t ntable = 0;
    int r = !boot_path(path, sizeof(path), "%s/eboot.bin", game) ? fail("path too long") : load_module(path, "eboot.bin", &main, NULL, NULL);
    if (!r) {
        size_t names = 0;
        r = prepare_eboot(&main, game, &p);
        if (!r) {
            names = p.nnames;
            printf("eboot.bin: entry=%#" PRIx64 ", image=%" PRIu64 " bytes, %zu imported symbols, %zu relocations; libc _init_env is RET: %s\n",
                   p.entry, p.size, names, p.nrelocs, p.flags ? "yes" : "no");
        }
    }
    if (!r) r = link_modules(&main, game, modules, &p, out_dir, table, &ntable, bases);
    if (!r) r = write_function_starts(&main.self, game, modules, bases, ntable, out_dir);
    if (!r) r = write_content_profile(game, out_dir);
    free_module(&main);
    free(p.image.data); free(p.segs); free(p.names); free(p.relocs);
    fflush(stdout);
    return r;
}
