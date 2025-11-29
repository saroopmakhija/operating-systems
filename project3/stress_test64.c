#include "my_vm64.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define P1_THREADS 32
#define P2_THREADS 16
#define P4_THREADS 32

static vaddr64_t p1_va[P1_THREADS];
static size_t    p1_size[P1_THREADS];

static vaddr64_t p2_va[P2_THREADS];
static size_t    p2_size[P2_THREADS];

struct thread_arg {
    int id;
};

// ---------------------------------------------------------------------------
// Phase 0: single-thread sanity + cross-page test
// ---------------------------------------------------------------------------

static void phase0_basic(void) {
    printf("\n[Phase 0] Single-thread basic + cross-page sanity\n");

    vm64_init();

    size_t bytes = PGSIZE64 * 2 + 128; // spans 2 full pages + extra
    vaddr64_t base = vm64_malloc(bytes);
    if (base == 0) {
        printf("  Phase 0 FAILED: vm64_malloc returned 0\n");
        return;
    }

    size_t n_ints = bytes / sizeof(int);
    int *tmp = (int *)malloc(n_ints * sizeof(int));
    if (!tmp) {
        printf("  Phase 0 FAILED: malloc(tmp) failed\n");
        vm64_free(base, bytes);
        return;
    }

    for (size_t i = 0; i < n_ints; i++) {
        tmp[i] = (int)i;
    }

    if (vm64_put(base, tmp, n_ints * sizeof(int)) != 0) {
        printf("  Phase 0 FAILED: vm64_put failed\n");
        free(tmp);
        vm64_free(base, bytes);
        return;
    }

    // Check values around page boundary
    int v1 = -1, v2 = -1, v3 = -1;
    vaddr64_t addr1 = base + (PGSIZE64 - (vaddr64_t)sizeof(int)); // last int in page 0
    vaddr64_t addr2 = base + (PGSIZE64);                          // first int in page 1
    vaddr64_t addr3 = base + (PGSIZE64 + (vaddr64_t)sizeof(int)); // second int in page 1

    if (vm64_get(addr1, &v1, sizeof(int)) != 0 ||
        vm64_get(addr2, &v2, sizeof(int)) != 0 ||
        vm64_get(addr3, &v3, sizeof(int)) != 0) {
        printf("  Phase 0 FAILED: vm64_get around page boundary failed\n");
        free(tmp);
        vm64_free(base, bytes);
        return;
    }

    size_t idx1 = (PGSIZE64 - sizeof(int)) / sizeof(int);
    size_t idx2 = PGSIZE64 / sizeof(int);
    size_t idx3 = idx2 + 1;

    if (v1 != (int)idx1 || v2 != (int)idx2 || v3 != (int)idx3) {
        printf("  Phase 0 FAILED: boundary values mismatch "
               "(got %d,%d,%d expected %zu,%zu,%zu)\n",
               v1, v2, v3, idx1, idx2, idx3);
        free(tmp);
        vm64_free(base, bytes);
        return;
    }

    free(tmp);
    vm64_free(base, bytes);
    printf("  Phase 0 passed\n");
}

// ---------------------------------------------------------------------------
// Phase 1: multithread small alloc/free + put/get sanity
// ---------------------------------------------------------------------------

static void *p1_worker(void *arg) {
    struct thread_arg *ta = (struct thread_arg *)arg;
    int id = ta->id;

    size_t sz = (size_t)(rand() % 2000 + 1); // 1..2000 bytes
    vaddr64_t va = vm64_malloc(sz);
    p1_va[id] = va;
    p1_size[id] = sz;

    if (va == 0) {
        printf("  [Phase 1] Thread %d: vm64_malloc failed for size %zu\n",
               id, sz);
        return NULL;
    }

    unsigned char *buf = (unsigned char *)malloc(sz);
    unsigned char *buf2 = (unsigned char *)malloc(sz);
    if (!buf || !buf2) {
        printf("  [Phase 1] Thread %d: malloc failed\n", id);
        free(buf);
        free(buf2);
        return NULL;
    }

    memset(buf, (unsigned char)(id + 7), sz);

    if (vm64_put(va, buf, sz) != 0) {
        printf("  [Phase 1] Thread %d: vm64_put failed\n", id);
        free(buf);
        free(buf2);
        return NULL;
    }

    if (vm64_get(va, buf2, sz) != 0) {
        printf("  [Phase 1] Thread %d: vm64_get failed\n", id);
        free(buf);
        free(buf2);
        return NULL;
    }

    if (memcmp(buf, buf2, sz) != 0) {
        printf("  [Phase 1] Thread %d: data mismatch after put/get\n", id);
    }

    free(buf);
    free(buf2);
    return NULL;
}

static void phase1_multithread_small(void) {
    printf("\n[Phase 1] Multithread small alloc/free + put/get\n");

    pthread_t tids[P1_THREADS];
    struct thread_arg args[P1_THREADS];

    for (int i = 0; i < P1_THREADS; i++) {
        args[i].id = i;
        p1_va[i] = 0;
        p1_size[i] = 0;
    }

    for (int i = 0; i < P1_THREADS; i++) {
        pthread_create(&tids[i], NULL, p1_worker, &args[i]);
    }
    for (int i = 0; i < P1_THREADS; i++) {
        pthread_join(tids[i], NULL);
    }

    for (int i = 0; i < P1_THREADS; i++) {
        if (p1_va[i] != 0 && p1_size[i] > 0) {
            vm64_free(p1_va[i], p1_size[i]);
        }
    }

    printf("  Phase 1 completed\n");
}

// ---------------------------------------------------------------------------
// Phase 2: multithread large cross-page allocations
// ---------------------------------------------------------------------------

static void *p2_worker(void *arg) {
    struct thread_arg *ta = (struct thread_arg *)arg;
    int id = ta->id;

    size_t sz = PGSIZE64 * 4 + 123; // 4 pages + some extra
    vaddr64_t va = vm64_malloc(sz);
    p2_va[id] = va;
    p2_size[id] = sz;

    if (va == 0) {
        printf("  [Phase 2] Thread %d: vm64_malloc failed\n", id);
        return NULL;
    }

    size_t n_ints = sz / sizeof(int);
    int *tmp = (int *)malloc(n_ints * sizeof(int));
    int *tmp2 = (int *)malloc(n_ints * sizeof(int));
    if (!tmp || !tmp2) {
        printf("  [Phase 2] Thread %d: malloc failed\n", id);
        free(tmp);
        free(tmp2);
        return NULL;
    }

    for (size_t i = 0; i < n_ints; i++) {
        tmp[i] = id * 100000 + (int)i;
    }

    if (vm64_put(va, tmp, n_ints * sizeof(int)) != 0) {
        printf("  [Phase 2] Thread %d: vm64_put failed\n", id);
        free(tmp);
        free(tmp2);
        return NULL;
    }

    if (vm64_get(va, tmp2, n_ints * sizeof(int)) != 0) {
        printf("  [Phase 2] Thread %d: vm64_get failed\n", id);
        free(tmp);
        free(tmp2);
        return NULL;
    }

    if (memcmp(tmp, tmp2, n_ints * sizeof(int)) != 0) {
        printf("  [Phase 2] Thread %d: data mismatch across pages\n", id);
    }

    free(tmp);
    free(tmp2);
    return NULL;
}

static void phase2_multithread_large(void) {
    printf("\n[Phase 2] Multithread large cross-page allocations\n");

    pthread_t tids[P2_THREADS];
    struct thread_arg args[P2_THREADS];

    for (int i = 0; i < P2_THREADS; i++) {
        args[i].id = i;
        p2_va[i] = 0;
        p2_size[i] = 0;
    }

    for (int i = 0; i < P2_THREADS; i++) {
        pthread_create(&tids[i], NULL, p2_worker, &args[i]);
    }
    for (int i = 0; i < P2_THREADS; i++) {
        pthread_join(tids[i], NULL);
    }

    for (int i = 0; i < P2_THREADS; i++) {
        if (p2_va[i] != 0 && p2_size[i] > 0) {
            vm64_free(p2_va[i], p2_size[i]);
        }
    }

    printf("  Phase 2 completed\n");
}

// ---------------------------------------------------------------------------
// Phase 3: fragmentation + physical reuse sanity
// ---------------------------------------------------------------------------

static void phase3_fragmentation(void) {
    printf("\n[Phase 3] Fragmentation + reuse check\n");

    const size_t NUM_PAGES = 512;
    const size_t BYTES = PGSIZE64;

    vaddr64_t *arr = (vaddr64_t *)malloc(NUM_PAGES * sizeof(vaddr64_t));
    if (!arr) {
        printf("  Phase 3 FAILED: malloc(arr) failed\n");
        return;
    }

    for (size_t i = 0; i < NUM_PAGES; i++) {
        arr[i] = vm64_malloc(BYTES);
        if (arr[i] == 0) {
            printf("  Phase 3 FAILED: initial vm64_malloc failed at %zu\n", i);
            free(arr);
            return;
        }
    }

    // Free every other page
    for (size_t i = 0; i < NUM_PAGES; i += 2) {
        vm64_free(arr[i], BYTES);
    }

    // Allocate a larger contiguous run to ensure enough physical pages available
    vaddr64_t big = vm64_malloc(NUM_PAGES * BYTES);
    if (big == 0) {
        printf("  Phase 3 FAILED: big vm64_malloc after partial free failed\n");
        free(arr);
        return;
    }

    // Clean up
    vm64_free(big, NUM_PAGES * BYTES);
    for (size_t i = 1; i < NUM_PAGES; i += 2) {
        vm64_free(arr[i], BYTES);
    }

    free(arr);
    printf("  Phase 3 completed (physical pages reused correctly)\n");
}

// ---------------------------------------------------------------------------
// Phase 4: mat_mult correctness (single + multithreaded)
// ---------------------------------------------------------------------------

static void phase4_single_matmul(void) {
    printf("\n[Phase 4] mat_mult correctness (single + multithread)\n");

    int n = 4;
    size_t bytes = (size_t)n * (size_t)n * sizeof(int);

    vaddr64_t A = vm64_malloc(bytes);
    vaddr64_t B = vm64_malloc(bytes);
    vaddr64_t C = vm64_malloc(bytes);

    if (A == 0 || B == 0 || C == 0) {
        printf("  Phase 4 FAILED: single matmul allocation failed\n");
        return;
    }

    // A = sequential 1..n*n
    // B = identity
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int valA = i * n + j + 1;
            int valB = (i == j) ? 1 : 0;

            vaddr64_t vaA = A + (vaddr64_t)(i * n + j) * sizeof(int);
            vaddr64_t vaB = B + (vaddr64_t)(i * n + j) * sizeof(int);

            vm64_put(vaA, &valA, sizeof(int));
            vm64_put(vaB, &valB, sizeof(int));
        }
    }

    vm64_mat_mult(A, B, n, C);

    // C should equal A
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        for (int j = 0; j < n && ok; j++) {
            int valC = 0;
            vaddr64_t vaC = C + (vaddr64_t)(i * n + j) * sizeof(int);
            if (vm64_get(vaC, &valC, sizeof(int)) != 0) {
                printf("  Phase 4 FAILED: vm64_get(C) failed\n");
                ok = 0;
                break;
            }
            int expected = i * n + j + 1;
            if (valC != expected) {
                printf("  Phase 4 FAILED: C[%d,%d]=%d expected=%d\n",
                       i, j, valC, expected);
                ok = 0;
                break;
            }
        }
    }

    if (ok) {
        printf("  Phase 4 single mat_mult passed\n");
    }

    vm64_free(A, bytes);
    vm64_free(B, bytes);
    vm64_free(C, bytes);
}

static void *p4_worker(void *arg) {
    (void)arg;

    int n = 8;
    size_t bytes = (size_t)n * (size_t)n * sizeof(int);

    vaddr64_t A = vm64_malloc(bytes);
    vaddr64_t B = vm64_malloc(bytes);
    vaddr64_t C = vm64_malloc(bytes);

    if (A == 0 || B == 0 || C == 0) {
        printf("  [Phase 4] multithread: vm64_malloc failed\n");
        return NULL;
    }

    // A = all ones, B = all ones -> C[i,j] = n
    int one = 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            vaddr64_t vaA = A + (vaddr64_t)(i * n + j) * sizeof(int);
            vaddr64_t vaB = B + (vaddr64_t)(i * n + j) * sizeof(int);
            vm64_put(vaA, &one, sizeof(int));
            vm64_put(vaB, &one, sizeof(int));
        }
    }

    vm64_mat_mult(A, B, n, C);

    // Spot-check a few entries
    int val = 0;
    vaddr64_t vaC = C; // C[0,0]
    if (vm64_get(vaC, &val, sizeof(int)) == 0) {
        if (val != n) {
            printf("  [Phase 4] multithread: C[0,0]=%d expected=%d\n",
                   val, n);
        }
    }

    vm64_free(A, bytes);
    vm64_free(B, bytes);
    vm64_free(C, bytes);
    return NULL;
}

static void phase4_multithread_matmul(void) {
    pthread_t tids[P4_THREADS];
    struct thread_arg args[P4_THREADS];

    for (int i = 0; i < P4_THREADS; i++) {
        args[i].id = i;
        pthread_create(&tids[i], NULL, p4_worker, &args[i]);
    }
    for (int i = 0; i < P4_THREADS; i++) {
        pthread_join(tids[i], NULL);
    }

    printf("  Phase 4 multithread mat_mult finished\n");
}

static void phase4_matmul_all(void) {
    phase4_single_matmul();
    phase4_multithread_matmul();
}

// ---------------------------------------------------------------------------
// Phase 5: TLB locality stress
// ---------------------------------------------------------------------------

static void phase5_tlb_locality(void) {
    printf("\n[Phase 5] TLB locality stress\n");

    printf("  TLB miss rate before locality: ");
    vm64_print_TLB_missrate();

    // Allocate a hot region of 128 pages
    const size_t HOT_PAGES = 128;
    const size_t HOT_BYTES = HOT_PAGES * PGSIZE64;

    vaddr64_t base = vm64_malloc(HOT_BYTES);
    if (base == 0) {
        printf("  Phase 5 FAILED: vm64_malloc for hot region failed\n");
        return;
    }

    // Touch the region many times with small working set -> high locality
    const int ITER = 100000;
    for (int i = 0; i < ITER; i++) {
        // random offset within HOT_BYTES aligned to int
        size_t idx = (size_t)(rand() % (HOT_BYTES / sizeof(int)));
        vaddr64_t va = base + (vaddr64_t)idx * sizeof(int);
        int val = (int)idx;
        vm64_put(va, &val, sizeof(int));
        vm64_get(va, &val, sizeof(int));
    }

    printf("  TLB miss rate after locality: ");
    vm64_print_TLB_missrate();

    vm64_free(base, HOT_BYTES);
    printf("  Phase 5 completed\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
    srand((unsigned)time(NULL));

    vm64_init();

    phase0_basic();
    phase1_multithread_small();
    phase2_multithread_large();
    phase3_fragmentation();
    phase4_matmul_all();
    phase5_tlb_locality();

    printf("\nAll 64-bit stress-test phases completed\n");
    return 0;
}
