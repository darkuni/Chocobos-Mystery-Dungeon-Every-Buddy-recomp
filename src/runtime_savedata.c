/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ftw.h>
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used; char host[700], meta[700]; } slots[SLOTS];
static char memory_path[SLOTS][720];  /* SaveDataMemory (slot 0) and SaveDataMemory2 slots */
static uint64_t memory_size[SLOTS];
static size_t mounts_done, memory_writes;

void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
/* GOW3_SAVEDATA_DIR: the saves in a folder of their own (a network share between two PCs, #86)
 * instead of <user>/savedata; the caches stay in the user folder. */
static int valid_name(const char *name, size_t max);
/* GOW3_PROFILE (launcher: Game > Profile, #162): that profile's saves in profiles/<name> of the
 * save data folder, so one backup of the folder holds every profile. */
static void root(int32_t user, const char *title, char *out, size_t size) {
    const char *own=getenv("GOW3_SAVEDATA_DIR"), *profile=getenv("GOW3_PROFILE");
    const char *sub=profile && valid_name(profile,64) ? "/profiles/" : "", *name=*sub ? profile : "";
    const char *game=title && *title ? title : title_id;
    if (own && *own) snprintf(out,size,"%s%s%s/%d/%s",own,sub,name,user,game);
    else snprintf(out,size,"%s/savedata%s%s/%d/%s",runtime_file_user_dir(),sub,name,user,game);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
static int write_param(const char *meta, const Param *p) {
    char path[700], pending[705];
    snprintf(path,sizeof(path),"%s/param.bin",meta);
    snprintf(pending,sizeof(pending),"%s.part",path);
    FILE *f=fopen(pending,"wb");
    if (!f) return -1;
    Param copy=*p; copy.mtime=time(NULL);
    int failed=fwrite(&copy,sizeof(copy),1,f)!=1;
    if (fclose(f)) failed=1;
    if (!failed) {
#ifdef _WIN32
        failed=!MoveFileExA(pending,path,MOVEFILE_REPLACE_EXISTING);
#else
        failed=rename(pending,path)!=0;
#endif
    }
    if (failed) remove(pending);
    return failed ? -1 : 0;
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
#ifdef _WIN32
static void remove_tree(const char *path) { compat_remove_tree(path); }
#else
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
static void remove_tree(const char *path) { nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
#endif

/* A save's param (title, subtitle, date) for the save dialog; 0 when it exists. */
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    if (!valid_name(dir,32)) return -1;
    char base[600], meta[680];
    root(user,NULL,base,sizeof(base));
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir);
    return read_param(meta,(Param *)param);
}
/* Host path of a save's icon0.png (the location picture in the save dialog); 0 when named. */
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) {
    if (!valid_name(dir,32)) return -1;
    char base[600];
    root(user,NULL,base,sizeof(base));
    snprintf(out,size,"%s/%s.sce_sys/icon0.png",base,dir);
    return 0;
}
static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    initialized=0; return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    int new_meta=stat(meta,&st)!=0 && errno==ENOENT;
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) {
        Param empty={0};
        if (write_param(meta,&empty)) {
            rmdir(meta); rmdir(host);
            pthread_mutex_unlock(&lock); return ERR_INTERNAL;
        }
    }
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    if (runtime_file_mount(point,host)) {
        if (!exists) {
            if (new_meta) {
                char param[720];
                snprintf(param,sizeof(param),"%s/param.bin",meta);
                unlink(param);
                rmdir(meta);
            }
            rmdir(host);
        }
        pthread_mutex_unlock(&lock);
        return ERR_INTERNAL;
    }
    slots[slot].used=1;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,m->title ? m->title->data : NULL,m->dir,m->mode,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,NULL,m->dir,m->mode,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_no_backup(const void *param) { return param ? ERR_NOT_FOUND : ERR_PARAMETER; }
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p;
    int read_failed=type && read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    int32_t r=(read_failed || write_param(slots[slot].meta,&p)) ? ERR_INTERNAL : 0;
    pthread_mutex_unlock(&lock);
    return r;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        FILE *f=fopen(path,"wb");
        r=f && fwrite(icon->buffer,1,icon->data_size,f)==icon->data_size ? 0 : ERR_INTERNAL;
        if (f) fclose(f);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    remove_tree(host);
    remove_tree(meta);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys"))) continue;
        if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
        if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); }
        snprintf(entries[count].name,32,"%s",e->d_name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
        read_param(meta,&entries[count].param);
        ++count;
    }
    if (d) closedir(d);
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title and slot (memory.dat, memory<slot>.dat). */
static int32_t memory_setup_slot(int32_t user, uint32_t slot, uint64_t size, const Param *param, const Icon *icon,
                                 uint64_t *existed) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size || slot>=SLOTS) return ERR_PARAMETER;
    char base[600], dir[640], path[720];
    root(user,NULL,base,sizeof(base));
    snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    if (slot) snprintf(path,sizeof(path),"%s/memory%u.dat",dir,slot);
    else snprintf(path,sizeof(path),"%s/memory.dat",dir);
    FILE *f=fopen(path,"r+b");
    if (!f) f=fopen(path,"w+b");
    int32_t r=ERR_INTERNAL;
    long length=-1;
    if (f) {
        if (fseek(f,0,SEEK_END)) goto done;
        length=ftell(f);
        if (length<0 || ((uint64_t)length<size && ftruncate(fileno(f),(off_t)size))) goto done;
        r=0;
    done:
        if (fclose(f)) r=ERR_INTERNAL;
    }
    if (existed) *existed=length>0 ? (uint64_t)length : 0;
    /* A new blob gets the game's param and icon; an existing one keeps its own. */
    if (!r && param && length<=0 && write_param(dir,param)) r=ERR_INTERNAL;
    if (!r && icon && icon->buffer && icon->data_size && length<=0) {
        char icon_path[740]; snprintf(icon_path,sizeof(icon_path),"%s/icon0.png",dir);
        FILE *g=fopen(icon_path,"wb");
        if (g) { fwrite(icon->buffer,1,icon->data_size,g); fclose(g); }
    }
    if (!r) {
        snprintf(memory_path[slot],sizeof(memory_path[slot]),"%s",path);
        memory_size[slot]=size;
    }
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory slot %u ready (%llu bytes, %s)\n",slot,(unsigned long long)size,
                   length>0 ? "existing" : "new");
    return r;
}
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    return memory_setup_slot(user,0,size,param,NULL,NULL);
}
static int32_t memory_io(uint32_t slot, void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0 || slot>=SLOTS) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memory_size[slot]) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memory_size[slot]) { pthread_mutex_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memory_path[slot],"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(0,buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(0,buffer,size,offset,1); }

/* SaveDataMemory2 (SDK 4.50+; slots from 5.00). Layouts as in shadPS4 savedata.cpp. */
typedef struct { void *buffer; uint64_t size; int64_t offset; uint8_t reserved[40]; } MemoryData;
typedef struct { uint32_t option; int32_t user; uint64_t memory_size, icon_size; const Param *param; const Icon *icon;
                 uint32_t slot; uint8_t reserved[20]; } MemorySetup2;
typedef struct { uint64_t existed; uint8_t reserved[16]; } MemorySetupResult;
typedef struct { int32_t user; uint8_t pad[4]; MemoryData *data; Param *param; Icon *icon; uint32_t slot; uint8_t reserved[28]; } MemoryGet2;
typedef struct { int32_t user; uint8_t pad[4]; const MemoryData *data; const Param *param; const Icon *icon;
                 uint32_t count, slot; uint8_t reserved[32]; } MemorySet2;
typedef struct { int32_t user; uint32_t slot, option; uint8_t reserved[28]; } MemorySync;
_Static_assert(sizeof(MemoryData)==64 && sizeof(MemorySetup2)==64 && sizeof(MemoryGet2)==64 && sizeof(MemorySet2)==72 &&
               sizeof(MemorySync)==40,"SaveDataMemory2 layouts");

/* Results of backups and memory syncs, for sceSaveDataGetEventResult. Saves are written straight
 * to disk, so every request has finished by the time it returns. */
typedef struct { uint32_t type; int32_t error; int32_t user; uint8_t pad[4]; TitleId title; DirName dir; uint8_t reserved[40]; } Event;
_Static_assert(sizeof(Event)==104,"OrbisSaveDataEvent layout");
static Event events[16];
static size_t event_count;
static void post_event(uint32_t type, int32_t user, const char *dir) {
    pthread_mutex_lock(&lock);
    if (event_count==sizeof(events)/sizeof(*events)) { memmove(events,events+1,sizeof(events)-sizeof(*events)); --event_count; }
    Event *e=&events[event_count++];
    memset(e,0,sizeof(*e));
    e->type=type; e->user=user;
    snprintf(e->title.data,sizeof(e->title.data),"%.9s",title_id);
    if (dir) snprintf(e->dir.data,sizeof(e->dir.data),"%.31s",dir);
    pthread_mutex_unlock(&lock);
}
static ABI int32_t memory_setup2(const MemorySetup2 *setup, MemorySetupResult *result) {
    if (!setup) return ERR_PARAMETER;
    uint64_t existed=0;
    int32_t r=memory_setup_slot(setup->user,setup->slot,setup->memory_size,setup->param,setup->icon,&existed);
    if (!r && result) { memset(result,0,sizeof(*result)); result->existed=existed; }
    return r;
}
static int read_file_into(const char *path, void *buffer, uint64_t capacity, uint64_t *size) {
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    *size=fread(buffer,1,capacity,f);
    fclose(f);
    return 0;
}
static void memory_dir(int32_t user, char *out, size_t size) {
    char base[600]; root(user,NULL,base,sizeof(base));
    snprintf(out,size,"%s.memory",base);
}
static ABI int32_t memory_get2(const MemoryGet2 *get) {
    if (!get) return ERR_PARAMETER;
    if (get->data) {
        int32_t r=memory_io(get->slot,get->data->buffer,get->data->size,get->data->offset,0);
        if (r) return r;
    } else if (get->slot>=SLOTS || !memory_size[get->slot]) return initialized ? ERR_MEMORY_NOT_READY : ERR_NOT_INITIALIZED;
    char dir[640]; memory_dir(get->user,dir,sizeof(dir));
    if (get->param && read_param(dir,get->param)) memset(get->param,0,sizeof(*get->param));
    if (get->icon && get->icon->buffer) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",dir);
        uint64_t n=0;
        Icon *icon=get->icon;
        if (read_file_into(path,(void *)icon->buffer,icon->buffer_size,&n)) n=0;
        icon->data_size=n;
    }
    return 0;
}
static ABI int32_t memory_set2(const MemorySet2 *set) {
    if (!set) return ERR_PARAMETER;
    for (uint32_t i=0;set->data && i<(set->count ? set->count : 1);++i) {
        int32_t r=memory_io(set->slot,set->data[i].buffer,set->data[i].size,set->data[i].offset,1);
        if (r) return r;
    }
    if (!set->data && (set->slot>=SLOTS || !memory_size[set->slot])) return initialized ? ERR_MEMORY_NOT_READY : ERR_NOT_INITIALIZED;
    char dir[640]; memory_dir(set->user,dir,sizeof(dir));
    if (set->param && write_param(dir,set->param)) return ERR_INTERNAL;
    if (set->icon && set->icon->buffer && set->icon->data_size) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",dir);
        FILE *f=fopen(path,"wb");
        if (!f) return ERR_INTERNAL;
        fwrite(set->icon->buffer,1,set->icon->data_size,f);
        fclose(f);
    }
    return 0;
}
static ABI int32_t memory_sync(const MemorySync *sync) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!sync) return ERR_PARAMETER;
    if (sync->slot>=SLOTS || !memory_size[sync->slot]) return ERR_MEMORY_NOT_READY;
    post_event(3 /* SAVE_DATA_MEMORY_SYNC */,sync->user,"sce_sdmemory");
    return 0;
}

/* ---- Calls of Unity's SaveData plugin ---- */
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint; uint8_t reserved[32]; } Backup;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } MountInfo;
static ABI int32_t save_backup(const Backup *b) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!b || !b->dir) return ERR_PARAMETER;
    post_event(2 /* BACKUP */,b->user,b->dir->data);
    return 0;
}
static ABI int32_t save_event_result(const void *param, Event *event) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!event || param) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int32_t r=ERR_NOT_FOUND;
    if (event_count) {
        *event=events[0];
        memmove(events,events+1,(event_count-1)*sizeof(*events));
        --event_count;
        r=0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_mount_info(const MountPoint *point, MountInfo *info) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !info) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    memset(info,0,sizeof(*info));
    info->blocks=32768; info->free_blocks=16384; /* as dir name search reports */
    return 0;
}
static ABI int32_t save_get_param(const MountPoint *point, uint32_t type, void *buffer, uint64_t size, uint64_t *got) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (type>5 || !buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    Param p;
    int missing=slot<0 ? 1 : read_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    if (missing) memset(&p,0,sizeof(p));
    const void *from=NULL; uint64_t n=0;
    switch (type) {
    case 0: from=&p; n=sizeof(p); break;
    case 1: from=p.title; n=strnlen(p.title,sizeof(p.title))+1; break;
    case 2: from=p.subtitle; n=strnlen(p.subtitle,sizeof(p.subtitle))+1; break;
    case 3: from=p.detail; n=strnlen(p.detail,sizeof(p.detail))+1; break;
    case 4: from=&p.user_param; n=4; break;
    case 5: from=&p.mtime; n=8; break;
    }
    if (n>size) n=size;
    memcpy(buffer,from,n);
    if (got) *got=n;
    return 0;
}
static ABI int32_t save_load_icon(const MountPoint *point, Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    char path[720];
    if (slot>=0) snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    uint64_t n=0;
    if (read_file_into(path,(void *)icon->buffer,icon->buffer_size,&n)) return ERR_NOT_FOUND;
    icon->data_size=n;
    return 0;
}
static ABI int32_t save_progress(float *progress) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!progress) return ERR_PARAMETER;
    *progress=1.0f;
    return 0;
}
static ABI int32_t save_clear_progress(void) { return initialized ? 0 : ERR_NOT_INITIALIZED; }
static ABI int32_t save_dialog_progress_inc(uint32_t target, uint32_t delta) { (void)target; (void)delta; return 0; }

static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",save_umount},
    /* No backup copies are kept (saves are written straight to disk): unmount normally, and
     * there is never a backup to check or restore. */
    {"sceSaveDataUmountWithBackup",save_umount},
    {"sceSaveDataCheckBackupData",save_no_backup}, {"sceSaveDataRestoreBackupData",save_no_backup},
    {"sceSaveDataSetParam",save_set_param}, {"sceSaveDataSaveIcon",save_icon}, {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
    {"sceSaveDataSetupSaveDataMemory2",memory_setup2}, {"sceSaveDataGetSaveDataMemory2",memory_get2},
    {"sceSaveDataSetSaveDataMemory2",memory_set2}, {"sceSaveDataSyncSaveDataMemory",memory_sync},
    {"sceSaveDataBackup",save_backup}, {"sceSaveDataGetEventResult",save_event_result},
    {"sceSaveDataGetMountInfo",save_mount_info}, {"sceSaveDataGetParam",save_get_param},
    {"sceSaveDataLoadIcon",save_load_icon}, {"sceSaveDataGetProgress",save_progress},
    {"sceSaveDataClearProgress",save_clear_progress}, {"sceSaveDataDialogProgressBarInc",save_dialog_progress_inc},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }

