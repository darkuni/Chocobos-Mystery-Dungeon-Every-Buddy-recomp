/* libSceFiber: cooperative user-mode threads (RESOGUN, CUSA00038).
 *
 * A fiber runs on a stack of its own and gives up the processor only by calling sceFiberSwitch
 * (to another fiber) or sceFiberReturnToThread (back to the sceFiberRun that started the chain).
 * The switch below saves the callee-saved registers of the System V ABI (the guest's and the
 * runtime's ABI for these entry points), MXCSR and the x87 control word, then changes stacks.
 *
 * The game's context buffer (addr_context/size_context) is kept in the fiber object but the
 * fiber runs on a stack the runtime allocates, as shadPS4 does on Windows: there an exception
 * is delivered on the faulting thread's stack, which must not be game memory the GPU cache may
 * write protect. Doing the same on Linux keeps both builds alike. On Windows the thread's stack
 * bounds in the TEB follow every switch, as the system's own fibers do.
 *
 * A fiber may resume on another thread than the one it left; its code reaches the per-thread
 * state through fiber_thread(), read again after every switch. A fiber's new state (idle, or
 * terminated when its entry returns) is published only after the switch has saved its stack. Layout, error codes and states follow shadPS4's fiber.h. */
#include "runtime.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define FIBER_ERROR_NULL ((int32_t)0x80590001)
#define FIBER_ERROR_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_ERROR_RANGE ((int32_t)0x80590003)
#define FIBER_ERROR_INVALID ((int32_t)0x80590004)
#define FIBER_ERROR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERROR_STATE ((int32_t)0x80590006)
#define FIBER_SIGNATURE0 0xdef1649cu
#define FIBER_SIGNATURE1 0xb37592a0u
#define FIBER_OPT_SIGNATURE 0xbb40e64du
#define FIBER_CONTEXT_MINIMUM 512
#define HOST_STACK_MINIMUM (256u * 1024u) /* runtime calls (logging, file I/O) also run on it */
enum { STATE_RUN = 1, STATE_IDLE = 2, STATE_TERMINATED = 3 };

typedef ABI void (*FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);

/* The guest's OrbisFiber (at most 256 bytes); `host` is where the PS4 keeps its context pointer. */
typedef struct HostFiber HostFiber;
typedef struct {
    uint32_t magic_start;
    volatile uint32_t state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[32];
    HostFiber *host;
    uint32_t flags;
    void *context_start;
    void *context_end;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber) <= 256, "OrbisFiber is 256 bytes at most");

struct HostFiber {
    void *sp;          /* saved stack pointer while the fiber is not running */
    unsigned char *stack;
    size_t stack_size;
    bool started;
    uint16_t fpucw;
    uint32_t mxcsr;
};

typedef struct {
    void *thread_sp;   /* the sceFiberRun caller, while a fiber runs on this thread */
    Fiber *current;
    uint64_t transfer; /* argument handed across the last switch */
    Fiber *release;    /* the fiber just left: its new state is published once its stack is saved */
    uint32_t release_state;
#ifdef _WIN32
    void *stack_base, *stack_limit;
#endif
} FiberThread;
static _Thread_local FiberThread fiber_thread_state;
/* The empty volatile asm keeps the compiler from treating this as a pure function and reusing
 * one thread's address after a switch that resumed on another thread. */
static __attribute__((noinline)) FiberThread *fiber_thread(void) {
    FiberThread *t = &fiber_thread_state;
    __asm__ volatile("" : "+r"(t));
    return t;
}
/* Runs first wherever execution continues after fiber_swap: only now is the left fiber's stack
 * saved, so only now may another thread run or finalize it. */
static void finish_switch(FiberThread *t) {
    if (t->release) {
        __atomic_store_n(&t->release->state, t->release_state, __ATOMIC_RELEASE);
        t->release = NULL;
    }
}

static uint64_t fibers_created, fiber_switches;

/* Saves rbp, rbx, r12-r15, MXCSR and the x87 control word on the current stack, stores the stack
 * pointer in *save and continues on `load` (a stack saved the same way, or a new fiber's frame). */
__attribute__((naked, noinline)) static ABI void fiber_swap(__attribute__((unused)) void **save, __attribute__((unused)) void *load) {
    __asm__ volatile(
        "push %rbp\n\tpush %rbx\n\tpush %r12\n\tpush %r13\n\tpush %r14\n\tpush %r15\n\t"
        "sub $16, %rsp\n\tstmxcsr 8(%rsp)\n\tfnstcw (%rsp)\n\t"
        "mov %rsp, (%rdi)\n\tmov %rsi, %rsp\n\t"
        "fldcw (%rsp)\n\tldmxcsr 8(%rsp)\n\tadd $16, %rsp\n\t"
        "pop %r15\n\tpop %r14\n\tpop %r13\n\tpop %r12\n\tpop %rbx\n\tpop %rbp\n\tret\n");
}

#ifdef _WIN32
static void set_stack_bounds(void *base, void *limit) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackBase = base;
    tib->StackLimit = limit;
}
#endif
static void enter_fiber_stack(HostFiber *h) {
#ifdef _WIN32
    set_stack_bounds(h->stack + h->stack_size, h->stack);
#else
    (void)h;
#endif
}
static void enter_thread_stack(FiberThread *t) {
#ifdef _WIN32
    set_stack_bounds(t->stack_base, t->stack_limit);
#else
    (void)t;
#endif
}

static ABI __attribute__((noreturn)) void fiber_start(void);

/* A frame fiber_swap returns from into fiber_start, with the System V stack alignment of a call. */
static void prepare_frame(HostFiber *h) {
    uintptr_t top = ((uintptr_t)(h->stack + h->stack_size)) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - 80);
    memset(frame, 0, 80);
    frame[0] = h->fpucw;            /* fldcw */
    frame[1] = h->mxcsr;            /* ldmxcsr */
    /* frame[2..7]: r15, r14, r13, r12, rbx, rbp = 0 */
    frame[8] = (uint64_t)(uintptr_t)fiber_start;
    h->sp = frame;
    h->started = true;
}

static ABI __attribute__((noreturn)) void fiber_start(void) {
    FiberThread *t = fiber_thread();
    finish_switch(t);
    Fiber *f = t->current;
    f->entry(f->arg_on_initialize, t->transfer);
    /* The entry returned: the PS4 ends the fiber and returns to the thread with 0. */
    t = fiber_thread();
    f = t->current;
    f->host->started = false;
    t->transfer = 0;
    t->release = f;
    t->release_state = STATE_TERMINATED;
    enter_thread_stack(t);
    void *discard;
    fiber_swap(&discard, t->thread_sp);
    __builtin_unreachable();
}

static int32_t check_fiber(const Fiber *f) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (f->magic_start != FIBER_SIGNATURE0 || f->magic_end != FIBER_SIGNATURE1 || !f->host) return FIBER_ERROR_INVALID;
    return 0;
}
static bool claim(Fiber *f) {
    return __atomic_compare_exchange_n(&f->state, &(uint32_t){STATE_IDLE}, STATE_RUN, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static ABI int32_t fiber_initialize(Fiber *f, const char *name, FiberEntry entry, uint64_t arg_on_initialize,
                                    void *addr_context, uint64_t size_context, const uint32_t *opt, uint32_t build_version) {
    if (!f || !name || !entry) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7 || (uintptr_t)addr_context & 15 || (uintptr_t)opt & 7) return FIBER_ERROR_ALIGNMENT;
    if (size_context && size_context < FIBER_CONTEXT_MINIMUM) return FIBER_ERROR_RANGE;
    if (size_context & 15 || !addr_context != !size_context) return FIBER_ERROR_INVALID;
    if (opt && *opt != FIBER_OPT_SIGNATURE) return FIBER_ERROR_INVALID;
    HostFiber *h = calloc(1, sizeof(HostFiber));
    if (!h) return FIBER_ERROR_RANGE;
    h->stack_size = size_context > HOST_STACK_MINIMUM ? (size_t)((size_context + 0xffff) & ~(uint64_t)0xffff) : HOST_STACK_MINIMUM;
#ifdef _WIN32
    h->stack = VirtualAlloc(NULL, h->stack_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    h->stack = mmap(NULL, h->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (h->stack == MAP_FAILED) h->stack = NULL;
#endif
    if (!h->stack) { free(h); return FIBER_ERROR_RANGE; }
    /* SDK 3.50 and later start fibers with the default FPU state; older ones inherit the caller's. */
    if (build_version >= 0x03500000) { h->fpucw = 0x037f; h->mxcsr = 0x9fc0; }
    else {
        uint32_t mxcsr; uint16_t cw;
        __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(mxcsr), "=m"(cw));
        h->fpucw = cw; h->mxcsr = mxcsr;
    }
    memset(f, 0, sizeof(*f));
    f->magic_start = FIBER_SIGNATURE0;
    f->magic_end = FIBER_SIGNATURE1;
    f->entry = entry;
    f->arg_on_initialize = arg_on_initialize;
    f->addr_context = addr_context;
    f->size_context = size_context;
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->host = h;
    f->context_start = h->stack;
    f->context_end = h->stack + h->stack_size;
    f->state = STATE_IDLE;
    ++fibers_created;
    return 0;
}

static ABI int32_t fiber_finalize(Fiber *f) {
    int32_t e = check_fiber(f);
    if (e) return e;
    uint32_t state = f->state;
    if ((state != STATE_IDLE && state != STATE_TERMINATED) ||
        !__atomic_compare_exchange_n(&f->state, &state, STATE_TERMINATED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return FIBER_ERROR_STATE;
    HostFiber *h = f->host;
#ifdef _WIN32
    VirtualFree(h->stack, 0, MEM_RELEASE);
#else
    munmap(h->stack, h->stack_size);
#endif
    free(h);
    f->host = NULL;
    f->magic_start = f->magic_end = 0;
    return 0;
}

/* Called on a thread outside any fiber: runs `f` until the chain returns to this thread. */
static ABI int32_t fiber_run(Fiber *f, uint64_t arg_on_run_to, uint64_t *arg_on_return) {
    int32_t e = check_fiber(f);
    if (e) return e;
    FiberThread *t = fiber_thread();
    if (t->current) return FIBER_ERROR_PERMISSION;
    if (!claim(f)) return FIBER_ERROR_STATE;
#ifdef _WIN32
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    t->stack_base = tib->StackBase;
    t->stack_limit = tib->StackLimit;
#endif
    HostFiber *h = f->host;
    if (!h->started) prepare_frame(h);
    t->current = f;
    t->transfer = arg_on_run_to;
    ++fiber_switches;
    enter_fiber_stack(h);
    fiber_swap(&t->thread_sp, h->sp);
    /* Back on this thread's own stack: the chain returned (or a fiber's entry ended). */
    t = fiber_thread();
    finish_switch(t);
    t->current = NULL;
    if (arg_on_return) *arg_on_return = t->transfer;
    return 0;
}

/* Called in a fiber: suspends it and continues `f` on this thread. */
static ABI int32_t fiber_switch(Fiber *f, uint64_t arg_on_run_to, uint64_t *arg_on_run) {
    int32_t e = check_fiber(f);
    if (e) return e;
    FiberThread *t = fiber_thread();
    Fiber *self = t->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    if (!claim(f)) return FIBER_ERROR_STATE;
    HostFiber *h = f->host;
    if (!h->started) prepare_frame(h);
    HostFiber *own = self->host;
    t->current = f;
    t->transfer = arg_on_run_to;
    ++fiber_switches;
    t->release = self;
    t->release_state = STATE_IDLE;
    enter_fiber_stack(h);
    fiber_swap(&own->sp, h->sp);
    /* Resumed, possibly on another thread. */
    t = fiber_thread();
    finish_switch(t);
    if (arg_on_run) *arg_on_run = t->transfer;
    return 0;
}

static ABI int32_t fiber_get_self(Fiber **out) {
    if (!out) return FIBER_ERROR_NULL;
    Fiber *self = fiber_thread()->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    *out = self;
    return 0;
}

/* Called in a fiber: suspends it and returns from the sceFiberRun of this thread. */
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return, uint64_t *arg_on_run) {
    FiberThread *t = fiber_thread();
    Fiber *self = t->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    HostFiber *own = self->host;
    t->transfer = arg_on_return;
    t->release = self;
    t->release_state = STATE_IDLE;
    enter_thread_stack(t);
    fiber_swap(&own->sp, t->thread_sp);
    t = fiber_thread();
    finish_switch(t);
    if (arg_on_run) *arg_on_run = t->transfer;
    return 0;
}

static const RuntimeExport exports[] = {
    {"_sceFiberInitializeImpl", fiber_initialize}, /* sceFiberInitialize (hVYD7Ou2pCQ) */
    {"sceFiberFinalize", fiber_finalize},
    {"sceFiberRun", fiber_run},
    {"sceFiberSwitch", fiber_switch},
    {"sceFiberGetSelf", fiber_get_self},
    {"sceFiberReturnToThread", fiber_return_to_thread},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
const char *runtime_fiber_current_name(void) {
    const Fiber *f = fiber_thread_state.current;
    return f ? f->name : NULL;
}
void runtime_fiber_report(void) {
    if (fibers_created)
        printf("Runtime: fibers created=%llu, switches=%llu\n", (unsigned long long)fibers_created, (unsigned long long)fiber_switches);
}
