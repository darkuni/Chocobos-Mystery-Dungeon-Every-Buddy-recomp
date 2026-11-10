/* libSceFiber (src/runtime_fiber.c): run, switch, return to the thread, an entry that returns,
 * a fiber resumed on another thread, the error results, and the FPU state across switches. */
#include "../src/runtime_fiber.c"
#include <assert.h>
#include <pthread.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, nid)) return (uintptr_t)table[i].function;
    return 0;
}

typedef struct { _Alignas(16) Fiber f; unsigned char context[4096] __attribute__((aligned(16))); } Slot;
static Slot a, b, c;
static uint64_t log_values[32];
static int log_count;
static void record(uint64_t v) { log_values[log_count++] = v; }

static ABI void entry_a(uint64_t init, uint64_t run) {
    record(init); record(run);                          /* 1, 100 */
    uint64_t got = 0;
    assert(fiber_return_to_thread(10, &got) == 0);      /* thread's run returns 10 */
    record(got);                                        /* 200 */
    Fiber *self = NULL;
    assert(fiber_get_self(&self) == 0 && self == &a.f);
    assert(fiber_run(&b.f, 0, NULL) == FIBER_ERROR_PERMISSION); /* already in a fiber */
    assert(fiber_switch(&b.f, 30, &got) == 0);          /* to b, then back here */
    record(got);                                        /* 50 */
    /* returning ends the fiber: the thread's run returns 0 */
}
static ABI void entry_b(uint64_t init, uint64_t run) {
    record(init); record(run);                          /* 2, 30 */
    uint64_t got = 0;
    assert(fiber_return_to_thread(40, &got) == 0);
    record(got);                                        /* 60 */
    assert(fiber_switch(&a.f, 0, NULL) == FIBER_ERROR_STATE); /* a has ended */
}

/* Fiber c returns to its thread, is resumed on another thread and checks the FPU state there. */
static ABI void entry_c(uint64_t init, uint64_t run) {
    (void)init; (void)run;
    uint32_t mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    record(mxcsr & 0xffc0);                             /* 0x9fc0: the default for SDK >= 3.50 */
    uint64_t got = 0;
    fiber_return_to_thread(1, &got);
    record(got);                                        /* 77, from the second thread */
}
static void *second_thread(void *arg) {
    (void)arg;
    uint64_t out = 99;
    assert(fiber_run(&c.f, 77, &out) == 0);
    assert(out == 0); /* entry returned */
    return NULL;
}

int main(void) {
    Fiber *none;
    assert(fiber_get_self(&none) == FIBER_ERROR_PERMISSION);
    assert(fiber_initialize(&a.f, "a", entry_a, 1, a.context, sizeof(a.context), NULL, 0x04008061) == 0);
    assert(fiber_initialize(&b.f, "b", entry_b, 2, b.context, sizeof(b.context), NULL, 0x04008061) == 0);
    assert(fiber_initialize(&c.f, "c", entry_c, 3, NULL, 0, NULL, 0x04008061) == 0);
    assert(fiber_initialize(&c.f, "bad", entry_c, 3, c.context, 100, NULL, 0) == FIBER_ERROR_RANGE);
    assert(fiber_initialize(&c.f, "bad", entry_c, 3, c.context + 8, 4096, NULL, 0) == FIBER_ERROR_ALIGNMENT);

    uint32_t thread_mxcsr = 0x1f80 | 0x2000; /* round down: must survive the switches */
    __asm__ volatile("ldmxcsr %0" : : "m"(thread_mxcsr));

    uint64_t out = 0;
    assert(fiber_run(&a.f, 100, &out) == 0 && out == 10);
    assert(fiber_run(&a.f, 200, &out) == 0 && out == 40); /* a switched to b, b returned 40 */
    assert(a.f.state == STATE_IDLE && b.f.state == STATE_IDLE);
    assert(fiber_run(&a.f, 50, &out) == 0 && out == 0);   /* a resumes in its switch, then ends */
    assert(a.f.state == STATE_TERMINATED);
    assert(fiber_run(&b.f, 60, &out) == 0 && out == 0);
    assert(fiber_finalize(&a.f) == 0 && fiber_finalize(&b.f) == 0);
    assert(fiber_run(&a.f, 0, NULL) == FIBER_ERROR_INVALID);

    uint32_t now;
    __asm__ volatile("stmxcsr %0" : "=m"(now));
    assert((now & 0xffc0) == (thread_mxcsr & 0xffc0));

    assert(fiber_run(&c.f, 5, &out) == 0 && out == 1);
    pthread_t t;
    pthread_create(&t, NULL, second_thread, NULL);
    pthread_join(t, NULL);
    assert(fiber_finalize(&c.f) == 0);

    const uint64_t expected[] = {1, 100, 200, 2, 30, 50, 60, 0x9fc0, 77};
    assert(log_count == (int)(sizeof(expected) / sizeof(*expected)));
    for (int i = 0; i < log_count; ++i) assert(log_values[i] == expected[i]);
    printf("fiber test passed (%llu switches)\n", (unsigned long long)fiber_switches);
    return 0;
}
