// stress_test64.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "../my_vm64.h"

// Tune these if you want even more punishment
#define PH1_THREADS       32
#define PH1_ITERS         256
#define PH1_MAX_OUT       8

#define PH2_THREADS       16
#define PH2_BLOCKS        4
#define PH2_BLOCK_PAGES   16   // each big block = 16 pages

#define FRAG_PAGES        1024

#define MAT_SINGLE_N      4
#define MAT_THREADS       16
#define MAT_MULTI_N       8

#define TLB_STRESS_PAGES  2048
#define TLB_RANDOM_ITERS  200000
#define TLB_LOCAL_ITERS   400000

// ---------- helpers ----------

static uint32_t urand32(unsigned int *seed) {
    return rand_r(seed);
}

// Host-side matmul for correctness checking
static void host_mat_mult_u32(const uint32_t *A,
                              const uint32_t *B,
                              uint32_t *C,
                              int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            uint64_t sum = 0;
            for (int k = 0; k < n; k++) {
                sum += (uint64_t)A[i*n + k] * (uint64_t)B[k*n + j];
            }
            C[i*n + j] = (uint32_t)sum;
        }
    }
}

// ===============================================================
// Phase 0: basic single-thread + cross-page sanity + invalid access
// ===============================================================

static void phase0_basic(void) {
    printf("\n[Phase 0] Single-thread basic + cross-page sanity (64-bit)\n");

    size_t bytes = PGSIZE64 * 2 + 128;   // spans >2 pages
    vaddr64_t base = vm64_malloc(bytes);
    if (!base) {
        printf("  Phase 0 FAILED: vm64_malloc returned 0\n");
        return;
    }

    size_t nints = bytes / sizeof(uint32_t);
    if (nints == 0) nints = 1;

    uint32_t *host = (uint32_t *)malloc(nints * sizeof(uint32_t));
    if (!host) {
        printf("  Phase 0 FAILED: malloc\n");
        vm64_free(base, bytes);
        return;
    }

    for (size_t i = 0; i < nints; i++) {
        host[i] = 0xDEAD0000u ^ (uint32_t)i;
    }

    if (vm64_put(base, host, bytes) != 0) {
        printf("  Phase 0 FAILED: vm64_put error\n");
        free(host);
        vm64_free(base, bytes);
        return;
    }

    // Check a few ints around each page boundary
    size_t ints_per_page = PGSIZE64 / sizeof(uint32_t);
    for (size_t page = 0; page < 2; page++) {
        if (ints_per_page < 4) break;
        size_t start = page * ints_per_page + ints_per_page - 4;
        for (size_t k = 0; k < 4; k++) {
            size_t idx = start + k;
            if (idx >= nints) break;
            uint32_t val = 0;
            vaddr64_t addr = base + idx * sizeof(uint32_t);
            if (vm64_get(addr, &val, sizeof(val)) != 0) {
                printf("  Phase 0 FAILED: vm64_get error at idx=%zu\n", idx);
                free(host);
                vm64_free(base, bytes);
                return;
            }
            if (val != host[idx]) {
                printf("  Phase 0 FAILED: mismatch at idx=%zu, got=0x%08x exp=0x%08x\n",
                       idx, val, host[idx]);
                free(host);
                vm64_free(base, bytes);
                return;
            }
        }
    }

    // Random spot checks
    for (int r = 0; r < 100; r++) {
        size_t idx = rand() % nints;
        uint32_t val = 0;
        vaddr64_t addr = base + idx * sizeof(uint32_t);
        if (vm64_get(addr, &val, sizeof(val)) != 0 || val != host[idx]) {
            printf("  Phase 0 FAILED: random mismatch at idx=%zu\n", idx);
            free(host);
            vm64_free(base, bytes);
            return;
        }
    }

    // Invalid access: definitely out of mapped range
    uint32_t bad = 0x12345678;
    vaddr64_t bad_va = base + bytes + PGSIZE64;
    int rc = vm64_put(bad_va, &bad, sizeof(bad));
    if (rc == 0) {
        printf("  [WARN] Phase 0: vm64_put unexpectedly succeeded on invalid VA\n");
    }

    free(host);
    vm64_free(base, bytes);
    printf("  Phase 0 passed\n");
}

// ===============================================================
// Phase 1: multithreaded random alloc/free + put/get
// ===============================================================

typedef struct {
    int tid;
    unsigned int seed;
} churn_arg_t;

static void *phase1_worker(void *arg) {
    churn_arg_t *a = (churn_arg_t *)arg;
    int tid = a->tid;
    unsigned int seed = a->seed;

    vaddr64_t slots[PH1_MAX_OUT] = {0};
    size_t sizes[PH1_MAX_OUT] = {0};

    for (int iter = 0; iter < PH1_ITERS; iter++) {
        int idx = iter % PH1_MAX_OUT;

        // Free an old allocation to churn
        if (slots[idx]) {
            vm64_free(slots[idx], sizes[idx]);
            slots[idx] = 0;
            sizes[idx] = 0;
        }

        size_t req = (size_t)(urand32(&seed) % (4 * PGSIZE64)) + 1;
        vaddr64_t base = vm64_malloc(req);
        if (!base) {
            fprintf(stderr, "  [Phase1][T%d] vm64_malloc failed at iter %d (req=%zu)\n",
                    tid, iter, req);
            continue;
        }

        // Fill with a pattern: val = pattern ^ offset-aligned
        uint32_t pattern = (uint32_t)(((uint32_t)tid << 16) ^ (uint32_t)iter);
        uint8_t *buf = (uint8_t *)malloc(req);
        if (!buf) {
            fprintf(stderr, "  [Phase1][T%d] host malloc failed\n", tid);
            vm64_free(base, req);
            continue;
        }

        for (size_t off = 0; off < req; off += 4) {
            uint32_t val = pattern ^ (uint32_t)(off & ~(size_t)3);
            size_t chunk = (req - off >= 4) ? 4 : (req - off);
            memcpy(buf + off, &val, chunk);
        }

        if (vm64_put(base, buf, req) != 0) {
            fprintf(stderr, "  [Phase1][T%d] vm64_put failed\n", tid);
            free(buf);
            vm64_free(base, req);
            continue;
        }

        // Verify 1 random byte
        size_t pos = (size_t)urand32(&seed) % req;
        uint8_t got = 0;
        if (vm64_get(base + pos, &got, 1) != 0) {
            fprintf(stderr, "  [Phase1][T%d] vm64_get failed\n", tid);
        } else {
            size_t off4 = pos & ~(size_t)3;
            uint32_t expected_val = pattern ^ (uint32_t)off4;
            uint8_t expected_byte = ((uint8_t *)&expected_val)[pos - off4];
            if (got != expected_byte) {
                fprintf(stderr,
                        "  [Phase1][T%d] data mismatch at iter=%d pos=%zu got=0x%02x exp=0x%02x\n",
                        tid, iter, pos, got, expected_byte);
            }
        }

        free(buf);
        slots[idx] = base;
        sizes[idx] = req;
    }

    // Free anything left
    for (int i = 0; i < PH1_MAX_OUT; i++) {
        if (slots[i]) {
            vm64_free(slots[i], sizes[i]);
        }
    }
    return NULL;
}

static void phase1_churn(void) {
    printf("\n[Phase 1] Multithread random alloc/free + put/get (64-bit)\n");

    pthread_t th[PH1_THREADS];
    churn_arg_t args[PH1_THREADS];

    for (int i = 0; i < PH1_THREADS; i++) {
        args[i].tid = i;
        args[i].seed = (unsigned int)time(NULL) ^ (unsigned int)(i * 7919);
        pthread_create(&th[i], NULL, phase1_worker, &args[i]);
    }
    for (int i = 0; i < PH1_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    printf("  Phase 1 completed\n");
}

// ===============================================================
// Phase 2: multithread large cross-page allocations
// ===============================================================

typedef struct {
    int tid;
} big_arg_t;

static void *phase2_worker(void *arg) {
    big_arg_t *a = (big_arg_t *)arg;
    int tid = a->tid;

    size_t block_bytes = (size_t)PH2_BLOCK_PAGES * PGSIZE64;

    for (int b = 0; b < PH2_BLOCKS; b++) {
        vaddr64_t base = vm64_malloc(block_bytes);
        if (!base) {
            fprintf(stderr, "  [Phase2][T%d] vm64_malloc failed for block %d\n", tid, b);
            continue;
        }

        // Write one uint64_t at start of each page with unique pattern
        for (int p = 0; p < PH2_BLOCK_PAGES; p++) {
            uint64_t pattern = ((uint64_t)tid << 32) ^ (uint64_t)((b << 16) | p);
            vaddr64_t addr = base + (size_t)p * PGSIZE64;
            if (vm64_put(addr, &pattern, sizeof(pattern)) != 0) {
                fprintf(stderr, "  [Phase2][T%d] vm64_put failed (b=%d p=%d)\n", tid, b, p);
            }
        }

        // Read back and verify
        for (int p = 0; p < PH2_BLOCK_PAGES; p++) {
            uint64_t pattern = ((uint64_t)tid << 32) ^ (uint64_t)((b << 16) | p);
            uint64_t got = 0;
            vaddr64_t addr = base + (size_t)p * PGSIZE64;
            if (vm64_get(addr, &got, sizeof(got)) != 0 || got != pattern) {
                fprintf(stderr,
                        "  [Phase2][T%d] mismatch b=%d p=%d got=0x%016" PRIx64
                        " exp=0x%016" PRIx64 "\n",
                        tid, b, p, got, pattern);
            }
        }

        vm64_free(base, block_bytes);
    }
    return NULL;
}

static void phase2_big_blocks(void) {
    printf("\n[Phase 2] Multithread large cross-page allocations (64-bit)\n");

    pthread_t th[PH2_THREADS];
    big_arg_t args[PH2_THREADS];

    for (int i = 0; i < PH2_THREADS; i++) {
        args[i].tid = i;
        pthread_create(&th[i], NULL, phase2_worker, &args[i]);
    }
    for (int i = 0; i < PH2_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    printf("  Phase 2 completed\n");
}

// ===============================================================
// Phase 3: fragmentation + reuse
// ===============================================================

static void phase3_fragmentation(void) {
    printf("\n[Phase 3] Fragmentation + reuse check (64-bit)\n");

    const size_t N = FRAG_PAGES;
    vaddr64_t *blocks = (vaddr64_t *)calloc(N, sizeof(vaddr64_t));
    vaddr64_t *blocks_copy = (vaddr64_t *)calloc(N, sizeof(vaddr64_t));
    if (!blocks || !blocks_copy) {
        printf("  Phase 3 FAILED: host alloc\n");
        free(blocks);
        free(blocks_copy);
        return;
    }

    size_t page_bytes = PGSIZE64;
    size_t allocated = 0;

    // Allocate N single-page blocks
    for (size_t i = 0; i < N; i++) {
        blocks[i] = vm64_malloc(page_bytes);
        if (!blocks[i]) break;
        allocated++;
        blocks_copy[i] = blocks[i];

        uint64_t mark = (uint64_t)0xCAFEBABE00000000ULL | (uint64_t)i;
        if (vm64_put(blocks[i], &mark, sizeof(mark)) != 0) {
            printf("  [Phase3] vm64_put failed at i=%zu\n", i);
        }
    }

    printf("  Allocated %zu single-page blocks\n", allocated);

    // Free every second block
    size_t freed_count = 0;
    for (size_t i = 0; i < allocated; i += 2) {
        if (blocks[i]) {
            vm64_free(blocks[i], page_bytes);
            blocks[i] = 0;
            freed_count++;
        }
    }

    // Allocate same number again
    vaddr64_t *new_blocks =
        (vaddr64_t *)calloc(freed_count > 0 ? freed_count : 1, sizeof(vaddr64_t));
    if (!new_blocks) {
        printf("  Phase 3 FAILED: new_blocks host alloc\n");
        for (size_t i = 0; i < allocated; i++) {
            if (blocks[i]) vm64_free(blocks[i], page_bytes);
        }
        free(blocks);
        free(blocks_copy);
        return;
    }

    size_t got_new = 0;
    for (size_t k = 0; k < freed_count; k++) {
        vaddr64_t va = vm64_malloc(page_bytes);
        if (!va) break;
        new_blocks[got_new++] = va;
    }

    // Count how many new blocks reused old freed VAs
    size_t reused = 0;
    for (size_t k = 0; k < got_new; k++) {
        vaddr64_t va = new_blocks[k];
        for (size_t i = 0; i < allocated; i += 2) {
            if (blocks_copy[i] && va == blocks_copy[i]) {
                reused++;
                break;
            }
        }
    }

    printf("  Phase 3 completed (allocated=%zu freed=%zu new=%zu reused=%zu)\n",
           allocated, freed_count, got_new, reused);

    // Cleanup
    for (size_t i = 0; i < allocated; i++) {
        if (blocks[i]) vm64_free(blocks[i], page_bytes);
    }
    for (size_t k = 0; k < got_new; k++) {
        if (new_blocks[k]) vm64_free(new_blocks[k], page_bytes);
    }

    free(new_blocks);
    free(blocks);
    free(blocks_copy);
}

// ===============================================================
// Phase 4: mat_mult correctness (single + multithread)
// ===============================================================

typedef struct {
    int idx;
    int n;
    vaddr64_t *A;
    vaddr64_t *B;
    vaddr64_t *C;
} mat_arg_t;

static void *mat_worker(void *arg) {
    mat_arg_t *m = (mat_arg_t *)arg;
    vm64_mat_mult(m->A[m->idx], m->B[m->idx], m->n, m->C[m->idx]);
    return NULL;
}

static void phase4_matmult(void) {
    printf("\n[Phase 4] mat_mult correctness (single + multithread, 64-bit)\n");

    // ----- Single-thread 4x4 A*I = A -----
    {
        const int n = MAT_SINGLE_N;
        size_t bytes = (size_t)n * n * sizeof(uint32_t);

        vaddr64_t A_va = vm64_malloc(bytes);
        vaddr64_t B_va = vm64_malloc(bytes);
        vaddr64_t C_va = vm64_malloc(bytes);

        if (!A_va || !B_va || !C_va) {
            printf("  Phase 4 single FAILED: vm64_malloc\n");
        } else {
            uint32_t A_host[n*n];
            uint32_t B_host[n*n];
            uint32_t C_host[n*n];
            uint32_t C_exp[n*n];

            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    A_host[i*n + j] = (uint32_t)(i + j + 1);
                    B_host[i*n + j] = (i == j) ? 1u : 0u; // identity
                }
            }

            vm64_put(A_va, A_host, bytes);
            vm64_put(B_va, B_host, bytes);

            vm64_mat_mult(A_va, B_va, n, C_va);
            vm64_get(C_va, C_host, bytes);

            host_mat_mult_u32(A_host, B_host, C_exp, n);

            int ok = 1;
            for (int i = 0; i < n*n; i++) {
                if (C_host[i] != C_exp[i]) {
                    ok = 0;
                    printf("  Phase 4 single FAILED at idx=%d got=%u exp=%u\n",
                           i, C_host[i], C_exp[i]);
                    break;
                }
            }
            if (ok) printf("  Phase 4 single 4x4 mat_mult passed\n");
        }

        if (A_va) vm64_free(A_va, bytes);
        if (B_va) vm64_free(B_va, bytes);
        if (C_va) vm64_free(C_va, bytes);
    }

    // ----- Multithread 8x8: A*I = A -----
    {
        const int n = MAT_MULTI_N;
        size_t bytes = (size_t)n * n * sizeof(uint32_t);
        vaddr64_t A_va[MAT_THREADS];
        vaddr64_t B_va[MAT_THREADS];
        vaddr64_t C_va[MAT_THREADS];
        uint32_t *A_host[MAT_THREADS];

        for (int t = 0; t < MAT_THREADS; t++) {
            A_va[t] = vm64_malloc(bytes);
            B_va[t] = vm64_malloc(bytes);
            C_va[t] = vm64_malloc(bytes);
            A_host[t] = (uint32_t *)malloc(bytes);
            if (!A_va[t] || !B_va[t] || !C_va[t] || !A_host[t]) {
                printf("  Phase 4 multi FAILED: alloc error at t=%d\n", t);
                for (int k = 0; k <= t; k++) {
                    if (A_va[k]) vm64_free(A_va[k], bytes);
                    if (B_va[k]) vm64_free(B_va[k], bytes);
                    if (C_va[k]) vm64_free(C_va[k], bytes);
                    free(A_host[k]);
                }
                return;
            }

            uint32_t B_host[n*n];
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    A_host[t][i*n + j] = (uint32_t)((t+1) * (i + j + 1));
                    B_host[i*n + j] = (i == j) ? 1u : 0u;
                }
            }
            vm64_put(A_va[t], A_host[t], bytes);
            vm64_put(B_va[t], B_host, bytes);
        }

        pthread_t th[MAT_THREADS];
        mat_arg_t args[MAT_THREADS];

        for (int t = 0; t < MAT_THREADS; t++) {
            args[t].idx = t;
            args[t].n = n;
            args[t].A = A_va;
            args[t].B = B_va;
            args[t].C = C_va;
            pthread_create(&th[t], NULL, mat_worker, &args[t]);
        }
        for (int t = 0; t < MAT_THREADS; t++) {
            pthread_join(th[t], NULL);
        }

        // Verify a couple of random matrices fully
        unsigned int seed = 1234567;
        for (int check = 0; check < 4; check++) {
            int t = (int)(urand32(&seed) % MAT_THREADS);
            uint32_t C_host[n*n];
            vm64_get(C_va[t], C_host, bytes);
            int ok = 1;
            for (int i = 0; i < n*n; i++) {
                if (C_host[i] != A_host[t][i]) {
                    ok = 0;
                    printf("  Phase 4 multi FAILED for t=%d at idx=%d got=%u exp=%u\n",
                           t, i, C_host[i], A_host[t][i]);
                    break;
                }
            }
            if (ok) {
                printf("  Phase 4 multi: thread %d 8x8 mat_mult passed\n", t);
            }
        }

        for (int t = 0; t < MAT_THREADS; t++) {
            vm64_free(A_va[t], bytes);
            vm64_free(B_va[t], bytes);
            vm64_free(C_va[t], bytes);
            free(A_host[t]);
        }
    }

    printf("  Phase 4 completed\n");
}

// ===============================================================
// Phase 5: TLB locality stress
// ===============================================================

static void phase5_tlb(void) {
    printf("\n[Phase 5] TLB locality stress (64-bit)\n");

    size_t pages = TLB_STRESS_PAGES;
    size_t bytes = pages * PGSIZE64;
    vaddr64_t base = vm64_malloc(bytes);
    if (!base) {
        printf("  Phase 5 FAILED: vm64_malloc\n");
        return;
    }

    uint32_t tmp = 0;

    // Touch each page once to ensure mappings exist
    for (size_t p = 0; p < pages; p++) {
        vaddr64_t addr = base + p * PGSIZE64;
        vm64_put(addr, &tmp, sizeof(tmp));
    }

    printf("  TLB miss rate before random access: ");
    vm64_print_TLB_missrate();

    // Random access across whole region (low locality)
    unsigned int seed = 0xC001C0DEu;
    for (size_t i = 0; i < TLB_RANDOM_ITERS; i++) {
        size_t p = urand32(&seed) % pages;
        vaddr64_t addr = base + p * PGSIZE64;
        vm64_get(addr, &tmp, sizeof(tmp));
    }
    printf("  TLB miss rate after random phase: ");
    vm64_print_TLB_missrate();

    // High locality: restrict to small hot set
    size_t hot_pages = 16;
    if (hot_pages > pages) hot_pages = pages;
    for (size_t i = 0; i < TLB_LOCAL_ITERS; i++) {
        size_t p = urand32(&seed) % hot_pages;
        vaddr64_t addr = base + p * PGSIZE64;
        vm64_get(addr, &tmp, sizeof(tmp));
    }
    printf("  TLB miss rate after locality phase: ");
    vm64_print_TLB_missrate();

    vm64_free(base, bytes);
    printf("  Phase 5 completed\n");
}

// ===============================================================
// main
// ===============================================================

int main(void) {
    srand((unsigned)time(NULL));

    // Initialize 64-bit VM once
    vm64_init();

    phase0_basic();
    phase1_churn();
    phase2_big_blocks();
    phase3_fragmentation();
    phase4_matmult();
    phase5_tlb();

    printf("\nAll 64-bit stress-test phases completed\n");
    return 0;
}
