/* sceKernelInstallExceptionHandler / sceKernelRaiseException: signals raised on another guest
 * thread, as Unity's garbage collector (Boehm, IL2CPP) uses them to stop the world: it raises
 * SIGUSR1 (30) on every thread and waits until each one's handler has run.
 *
 * Delivery follows POSIX signals:
 * - A thread running guest code is interrupted there. Windows: suspended, its context saved on its
 *   own stack below the red zone, resumed in hijack_entry, which calls the handler and then
 *   restores the exact context (RtlRestoreContext). Linux: a host real-time signal whose handler
 *   calls the guest handler.
 * - A thread blocked in a runtime wait (semaphores, event flags, condition variables, mutexes,
 *   sleeps, event queues) gets it from the wait, which runs in short slices and calls
 *   runtime_signal_poll() with its locks released; sem_wait then returns EINTR.
 * - A thread briefly inside other runtime code is retried by a delivery thread until it is back in
 *   guest code or in a wait.
 * The handler gets a PS4 ucontext (0x500 bytes, mcontext at 0x40, layout as in shadPS4). */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <ucontext.h>
#endif

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
#define SIGNALS 128

typedef struct {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    int32_t trapno; uint16_t fs, gs;
    uint64_t addr;
    int32_t flags; uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss, len, fpformat, ownedfp, lbrfrom, lbrto, aux1, aux2;
    uint64_t fpstate[104];
    uint64_t fsbase, gsbase, spare[6];
} Mcontext;
typedef struct {
    uint32_t sigmask[4];
    int32_t field1[12];
    Mcontext mcontext;
    void *link;
    void *ss_sp; uint64_t ss_size; int32_t ss_flags, ss_align;
    int32_t flags, spare[4], field7[3];
} Ucontext;
_Static_assert(sizeof(Mcontext) == 0x480 && sizeof(Ucontext) == 0x500, "PS4 ucontext layout");

typedef struct SigThread {
    void *guest;               /* the guest's pthread handle */
    volatile int32_t pending;  /* signal number waiting for delivery, 0 = none */
#ifdef _WIN32
    HANDLE host;
#else
    pthread_t host;
#endif
    struct SigThread *next;
} SigThread;

static SigThread *threads;
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local SigThread *self;
static void *volatile handlers[SIGNALS];
static uintptr_t guest_lo, guest_hi;
static volatile uint64_t raised, delivered;

typedef void (ABI *GuestHandler)(int, void *);

void runtime_signal_set_guest_range(const void *start, uint64_t size) {
    guest_lo = (uintptr_t)start;
    guest_hi = guest_lo + size;
}
static int in_guest(uintptr_t rip) { return rip >= guest_lo && rip < guest_hi; }

/* Called by every thread that runs guest code (runtime_thread.c attach). */
void runtime_signal_attach(void *guest) {
    SigThread *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->guest = guest;
#ifdef _WIN32
    t->host = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, GetCurrentThreadId());
#else
    t->host = pthread_self();
#endif
    pthread_mutex_lock(&threads_lock);
    t->next = threads;
    threads = t;
    pthread_mutex_unlock(&threads_lock);
    self = t;
}
static SigThread *find(void *guest) {
    pthread_mutex_lock(&threads_lock);
    SigThread *t = threads;
    while (t && t->guest != guest) t = t->next;
    pthread_mutex_unlock(&threads_lock);
    return t;
}

static void call_handler(int sig, Ucontext *uc) {
    GuestHandler h = (GuestHandler)handlers[sig];
    if (h) h(sig, uc);
    __atomic_add_fetch(&delivered, 1, __ATOMIC_RELAXED);
}

int runtime_signal_pending(void) { return self && self->pending; }
/* Deliver this thread's pending signal from host code (a wait, with its locks released). The
 * guest's registers are wherever the runtime left them; rsp is the current stack, which holds
 * every guest frame of this thread. */
int runtime_signal_poll(void) {
    if (!self || !self->pending) return 0;
    int sig = __atomic_exchange_n(&self->pending, 0, __ATOMIC_ACQ_REL);
    if (!sig) return 0;
    Ucontext uc;
    memset(&uc, 0, sizeof(uc));
    uc.mcontext.rsp = (uint64_t)(uintptr_t)&uc;
    call_handler(sig, &uc);
    return 1;
}

#ifdef _WIN32
typedef VOID (NTAPI *RestoreContext)(PCONTEXT, PEXCEPTION_RECORD);
static RestoreContext restore_context;
typedef struct {
    CONTEXT context; /* first: 16-byte aligned */
    int sig;
} Hijack;
static void fill(Ucontext *uc, const CONTEXT *c) {
    memset(uc, 0, sizeof(*uc));
    Mcontext *m = &uc->mcontext;
    m->rdi = c->Rdi; m->rsi = c->Rsi; m->rdx = c->Rdx; m->rcx = c->Rcx; m->r8 = c->R8; m->r9 = c->R9;
    m->rax = c->Rax; m->rbx = c->Rbx; m->rbp = c->Rbp; m->r10 = c->R10; m->r11 = c->R11; m->r12 = c->R12;
    m->r13 = c->R13; m->r14 = c->R14; m->r15 = c->R15; m->rip = c->Rip; m->rsp = c->Rsp;
    m->rflags = c->EFlags; m->cs = c->SegCs; m->ss = c->SegSs; m->fs = (uint16_t)c->SegFs; m->gs = (uint16_t)c->SegGs;
    m->len = sizeof(Mcontext);
}
/* Entered on the interrupted thread with its saved context below the red zone. */
static void hijack_entry(Hijack *h) {
    Ucontext uc;
    fill(&uc, &h->context);
    call_handler(h->sig, &uc);
    restore_context(&h->context, NULL);
}
/* 1 when the signal was delivered (or nothing was pending), 0 when the thread is in host code. */
static int try_deliver(SigThread *t) {
    if (!t->pending) return 1;
    if (!t->host || !restore_context) return 0;
    if (SuspendThread(t->host) == (DWORD)-1) return 0;
    static __thread CONTEXT c __attribute__((aligned(16)));
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    int done = 0;
    if (GetThreadContext(t->host, &c) && in_guest(c.Rip)) {
        int sig = __atomic_exchange_n(&t->pending, 0, __ATOMIC_ACQ_REL);
        if (!sig) done = 1;
        else {
            uintptr_t sp = ((uintptr_t)c.Rsp - 128 - sizeof(Hijack)) & ~(uintptr_t)63;
            Hijack *h = (Hijack *)sp;
            memcpy(&h->context, &c, sizeof(c));
            h->sig = sig;
            CONTEXT n = c;
            n.Rip = (DWORD64)(uintptr_t)hijack_entry;
            n.Rcx = (DWORD64)sp;
            n.Rsp = (DWORD64)(sp - 40); /* return slot + 32-byte shadow space, rsp % 16 == 8 */
            *(uint64_t *)(sp - 40) = 0;
            if (SetThreadContext(t->host, &n)) done = 1;
            else t->pending = sig;
        }
    }
    ResumeThread(t->host);
    return done;
}
#else
#define HIJACK_SIGNAL (SIGRTMIN + 5)
static void on_hijack(int s, siginfo_t *info, void *raw) {
    (void)s; (void)info;
    ucontext_t *u = raw;
    if (!self || !self->pending || !in_guest((uintptr_t)u->uc_mcontext.gregs[REG_RIP])) return;
    int sig = __atomic_exchange_n(&self->pending, 0, __ATOMIC_ACQ_REL);
    if (!sig) return;
    const greg_t *g = u->uc_mcontext.gregs;
    Ucontext uc;
    memset(&uc, 0, sizeof(uc));
    Mcontext *m = &uc.mcontext;
    m->rdi = g[REG_RDI]; m->rsi = g[REG_RSI]; m->rdx = g[REG_RDX]; m->rcx = g[REG_RCX]; m->r8 = g[REG_R8];
    m->r9 = g[REG_R9]; m->rax = g[REG_RAX]; m->rbx = g[REG_RBX]; m->rbp = g[REG_RBP]; m->r10 = g[REG_R10];
    m->r11 = g[REG_R11]; m->r12 = g[REG_R12]; m->r13 = g[REG_R13]; m->r14 = g[REG_R14]; m->r15 = g[REG_R15];
    m->rip = g[REG_RIP]; m->rsp = g[REG_RSP]; m->rflags = g[REG_EFL]; m->len = sizeof(Mcontext);
    call_handler(sig, &uc);
}
static int try_deliver(SigThread *t) {
    if (!t->pending) return 1;
    pthread_kill(t->host, HIJACK_SIGNAL);
    return !t->pending;
}
#endif

/* Retries signals whose thread was in host code (not in a wait) when they were raised. */
static pthread_mutex_t retry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t retry_wake = PTHREAD_COND_INITIALIZER;
static int retry_started, retry_requests;
static void *retry_thread(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&retry_lock);
        while (!retry_requests) pthread_cond_wait(&retry_wake, &retry_lock);
        retry_requests = 0;
        pthread_mutex_unlock(&retry_lock);
        for (int round = 0; round < 20000; ++round) {
            int waiting = 0;
            pthread_mutex_lock(&threads_lock);
            for (SigThread *t = threads; t; t = t->next)
                if (t->pending && !try_deliver(t)) waiting = 1;
            pthread_mutex_unlock(&threads_lock);
            if (!waiting) break;
            struct timespec pause = {0, 200000};
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}
static void request_retry(void) {
    pthread_mutex_lock(&retry_lock);
    if (!retry_started) {
        pthread_t id;
        if (!pthread_create(&id, NULL, retry_thread, NULL)) { pthread_detach(id); retry_started = 1; }
    }
    retry_requests = 1;
    pthread_cond_signal(&retry_wake);
    pthread_mutex_unlock(&retry_lock);
}

static ABI int32_t install_exception_handler(int sig, void *handler) {
    if (sig < 1 || sig >= SIGNALS) return ERR(22);
#ifdef _WIN32
    if (!restore_context)
        restore_context = (RestoreContext)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlRestoreContext");
#else
    static int installed;
    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_hijack;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(HIJACK_SIGNAL, &sa, NULL);
        installed = 1;
    }
#endif
    handlers[sig] = handler;
    printf("Runtime: exception handler for signal %d installed\n", sig);
    return 0;
}
static ABI int32_t remove_exception_handler(int sig) {
    if (sig < 1 || sig >= SIGNALS) return ERR(22);
    handlers[sig] = NULL;
    return 0;
}
static ABI int32_t raise_exception(void *thread, int sig) {
    if (sig < 1 || sig >= SIGNALS) return ERR(22);
    SigThread *t = find(thread);
    if (!t) { printf("Runtime: sceKernelRaiseException(%p, %d): unknown thread\n", thread, sig); return ERR(3); }
    if (!handlers[sig]) return 0;
    uint64_t n = __atomic_add_fetch(&raised, 1, __ATOMIC_RELAXED);
    if (n <= 4 || !(n & (n - 1)))
        printf("Runtime: signal %d raised %llu times, delivered %llu\n", sig, (unsigned long long)n,
               (unsigned long long)delivered);
    __atomic_store_n(&t->pending, sig, __ATOMIC_RELEASE);
    if (t == self) { runtime_signal_poll(); return 0; }
    if (!try_deliver(t)) request_retry();
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceKernelInstallExceptionHandler", install_exception_handler},
    {"sceKernelRemoveExceptionHandler", remove_exception_handler},
    {"sceKernelRaiseException", raise_exception},
};
uintptr_t runtime_signal_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
