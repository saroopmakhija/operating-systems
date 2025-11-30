#include "../my_vm3.h"
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * Heavy stress test for my_vm3 (fragmentation-aware allocator)
 *
 * Phases:
 *   0. Cross-page and misaligned put_data/get_data
 *   1. Large single-threaded matrix multiplies (slow)
 *   2. Multithreaded medium matrix multiplies
 *   3. TLB thrash over large working set
 *   4. TLB locality test on a small hot set
 *   5. Mixed small and large allocations with random frees
 */

#define SMALL_SZ             64
#define MEDIUM_SZ            2048
#define BIG_REGION_PAGES     4096   /* 4096 pages = 256MB if PGSIZE=64KB */
#define HEAVY_MAT_N1         192
#define HEAVY_MAT_N2         256
#define THREAD_MAT_N         96
#define THREAD_COUNT         4
#define MIX_ALLOCS           4096
#define MIX_MAX_LIVE         2048

/* --------------------------------------------------------------------------
 * Phase 0: Cross-page and misaligned put_data/get_data
 * -------------------------------------------------------------------------- */

static void phase0_cross_page_copy(void) {
    printf("[Phase 0] Cross-page and misaligned put_data/get_data\n");

    /* Make a big region that will cross many pages */
    size_t big_bytes = PGSIZE * 10 + 123;
    void *base = n_malloc((unsigned int)(big_bytes + 128));
    if (!base) {
        printf("  n_malloc for big region failed\n\n");
        return;
    }

    /* Use a misaligned virtual address inside that region */
    uint32_t base_u = VA2U(base);
    uint32_t misaligned_u = base_u + 37;
    void *misaligned = U2VA(misaligned_u);

    uint8_t *src = malloc(big_bytes);
    uint8_t *dst = malloc(big_bytes);
    if (!src || !dst) {
        printf("  malloc failed for temp buffers\n");
        if (src) free(src);
        if (dst) free(dst);
        n_free(base, (int)(big_bytes + 128));
        return;
    }

    for (size_t i = 0; i < big_bytes; i++) {
        src[i] = (uint8_t)((i * 131u + 17u) % 251u);
    }

    if (put_data(misaligned, src, (int)big_bytes) != 0) {
        printf("  put_data failed for misaligned cross-page region\n");
        free(src);
        free(dst);
        n_free(base, (int)(big_bytes + 128));
        printf("  Phase 0 completed (with errors)\n\n");
        return;
    }

    memset(dst, 0, big_bytes);
    get_data(misaligned, dst, (int)big_bytes);

    int mismatch = 0;
    for (size_t i = 0; i < big_bytes; i++) {
        if (src[i] != dst[i]) {
            printf("  MISMATCH at byte %zu: got %u expected %u\n",
                   i, (unsigned)dst[i], (unsigned)src[i]);
            mismatch = 1;
            break;
        }
    }

    if (!mismatch) {
        printf("  Cross-page misaligned put/get verified for %zu bytes\n", big_bytes);
    }

    free(src);
    free(dst);
    n_free(base, (int)(big_bytes + 128));

    printf("  Phase 0 completed\n\n");
}

/* --------------------------------------------------------------------------
 * Phase 1: Heavy single-threaded matrix multiply
 * -------------------------------------------------------------------------- */

static int heavy_mat_mult_once(int N) {
    printf("  [Phase 1] heavy_mat_mult_once N=%d\n", N);

    size_t elems = (size_t)N * (size_t)N;
    size_t bytes = elems * sizeof(uint32_t);

    void *A = n_malloc((unsigned int)bytes);
    void *B = n_malloc((unsigned int)bytes);
    void *C = n_malloc((unsigned int)bytes);

    if (!A || !B || !C) {
        printf("    n_malloc failed for matrices\n");
        if (A) n_free(A, (int)bytes);
        if (B) n_free(B, (int)bytes);
        if (C) n_free(C, (int)bytes);
        return -1;
    }

    uint32_t *Aloc = malloc(bytes);
    uint32_t *Bloc = malloc(bytes);
    uint32_t *Cloc = malloc(bytes);

    if (!Aloc || !Bloc || !Cloc) {
        printf("    malloc failed for local matrices\n");
        if (Aloc) free(Aloc);
        if (Bloc) free(Bloc);
        if (Cloc) free(Cloc);
        n_free(A, (int)bytes);
        n_free(B, (int)bytes);
        n_free(C, (int)bytes);
        return -1;
    }

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint32_t a = (uint32_t)(i + j);
            uint32_t b = (uint32_t)(i == j ? 2 : 1);
            Aloc[i * N + j] = a;
            Bloc[i * N + j] = b;
        }
    }

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint32_t a = Aloc[i * N + j];
            uint32_t b = Bloc[i * N + j];
            uint32_t offA = (uint32_t)((i * N + j) * sizeof(uint32_t));
            uint32_t offB = (uint32_t)((i * N + j) * sizeof(uint32_t));
            put_data(U2VA(VA2U(A) + offA), &a, sizeof(uint32_t));
            put_data(U2VA(VA2U(B) + offB), &b, sizeof(uint32_t));
        }
    }

    mat_mult(A, B, N, C);

    for (size_t idx = 0; idx < elems; idx++) {
        uint32_t tmp = 0;
        uint32_t off = (uint32_t)(idx * sizeof(uint32_t));
        get_data(U2VA(VA2U(C) + off), &tmp, sizeof(uint32_t));
        Cloc[idx] = tmp;
    }

    int bad = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint64_t sum = 0;
            for (int k = 0; k < N; k++) {
                uint32_t a = Aloc[i * N + k];
                uint32_t b = Bloc[k * N + j];
                sum += (uint64_t)a * (uint64_t)b;
            }
            uint32_t expect = (uint32_t)sum;
            uint32_t got = Cloc[i * N + j];
            if (got != expect) {
                printf("    MISMATCH at (%d,%d): got=%u expect=%u\n",
                       i, j, got, expect);
                bad = 1;
                goto done_check;
            }
        }
    }

done_check:
    if (!bad) {
        printf("    heavy_mat_mult_once N=%d verified OK\n", N);
    }

    free(Aloc);
    free(Bloc);
    free(Cloc);

    n_free(A, (int)bytes);
    n_free(B, (int)bytes);
    n_free(C, (int)bytes);

    return bad ? -1 : 0;
}

static void phase1_heavy_mat_mult(void) {
    printf("[Phase 1] Large single-threaded matrix multiplications\n");

    heavy_mat_mult_once(HEAVY_MAT_N1);
    heavy_mat_mult_once(HEAVY_MAT_N2);

    printf("  Note: increase HEAVY_MAT_N1/N2 in stresstest2.c if you want it even slower.\n");
    printf("  Phase 1 completed\n\n");
}

/* --------------------------------------------------------------------------
 * Phase 2: Multithread medium matrix multiplies
 * -------------------------------------------------------------------------- */

typedef struct {
    int tid;
} mat_thread_arg_t;

static void *thread_mat_worker(void *arg) {
    mat_thread_arg_t *t = (mat_thread_arg_t *)arg;
    int tid = t->tid;

    int N = THREAD_MAT_N;
    size_t elems = (size_t)N * (size_t)N;
    size_t bytes = elems * sizeof(uint32_t);

    void *A = n_malloc((unsigned int)bytes);
    void *B = n_malloc((unsigned int)bytes);
    void *C = n_malloc((unsigned int)bytes);

    if (!A || !B || !C) {
        printf("  [thread %d] n_malloc failed for matrices\n", tid);
        if (A) n_free(A, (int)bytes);
        if (B) n_free(B, (int)bytes);
        if (C) n_free(C, (int)bytes);
        return NULL;
    }

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint32_t a = (uint32_t)(i + 2 * tid);
            uint32_t b = (uint32_t)(j + tid);
            uint32_t off = (uint32_t)((i * N + j) * sizeof(uint32_t));
            put_data(U2VA(VA2U(A) + off), &a, sizeof(uint32_t));
            put_data(U2VA(VA2U(B) + off), &b, sizeof(uint32_t));
        }
    }

    mat_mult(A, B, N, C);

    uint32_t sample = 0;
    uint32_t off = (uint32_t)( (0 * N + 0) * sizeof(uint32_t) );
    get_data(U2VA(VA2U(C) + off), &sample, sizeof(uint32_t));
    printf("  [thread %d] sample C[0,0] = %u\n", tid, sample);

    n_free(A, (int)bytes);
    n_free(B, (int)bytes);
    n_free(C, (int)bytes);

    return NULL;
}

static void phase2_multithread_mat(void) {
    printf("[Phase 2] Multithreaded medium matrix multiplications\n");

    pthread_t th[THREAD_COUNT];
    mat_thread_arg_t args[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++) {
        args[i].tid = i;
        if (pthread_create(&th[i], NULL, thread_mat_worker, &args[i]) != 0) {
            perror("  pthread_create");
            exit(1);
        }
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(th[i], NULL);
    }

    printf("  Phase 2 completed\n\n");
}

/* --------------------------------------------------------------------------
 * Phase 3 and 4: TLB thrash and locality
 * -------------------------------------------------------------------------- */

static void phase3_tlb_thrash(void) {
    printf("[Phase 3] TLB thrash with large working set\n");

    uint32_t region_pages = BIG_REGION_PAGES;
    size_t bytes = (size_t)region_pages * (size_t)PGSIZE;

    void *base = n_malloc((unsigned int)bytes);
    if (!base) {
        printf("  n_malloc failed for TLB region (bytes=%zu)\n", bytes);
        printf("  Phase 3 skipped\n\n");
        return;
    }

    printf("  Region base VA=%p pages=%u\n", base, region_pages);

    printf("  TLB miss rate before thrash: ");
    print_TLB_missrate();

    int rounds = 8;
    for (int r = 0; r < rounds; r++) {
        for (uint32_t p = 0; p < region_pages; p++) {
            uint32_t v = (uint32_t)(r ^ p);
            uint32_t off = (uint32_t)(p * PGSIZE);
            void *addr = U2VA(VA2U(base) + off);
            put_data(addr, &v, sizeof(uint32_t));
        }
    }

    printf("  TLB miss rate after thrash:  ");
    print_TLB_missrate();

    n_free(base, (int)bytes);

    printf("  Phase 3 completed\n\n");
}

static void phase4_tlb_locality(void) {
    printf("[Phase 4] TLB locality on small hot set\n");

    int hot_pages = 32;
    size_t bytes = (size_t)hot_pages * (size_t)PGSIZE;

    void *base = n_malloc((unsigned int)bytes);
    if (!base) {
        printf("  n_malloc failed for hot-set region\n");
        printf("  Phase 4 skipped\n\n");
        return;
    }

    printf("  Hot region base VA=%p pages=%d\n", base, hot_pages);

    printf("  TLB miss rate before locality: ");
    print_TLB_missrate();

    for (int r = 0; r < 2000; r++) {
        for (int p = 0; p < hot_pages; p++) {
            uint32_t value = (uint32_t)(r + p);
            uint32_t off = (uint32_t)(p * PGSIZE);
            void *addr = U2VA(VA2U(base) + off);
            put_data(addr, &value, sizeof(uint32_t));
            uint32_t check = 0;
            get_data(addr, &check, sizeof(uint32_t));
            if (check != value) {
                printf("  Locality mismatch at r=%d p=%d: got=%u expect=%u\n",
                       r, p, check, value);
            }
        }
    }

    printf("  TLB miss rate after locality:  ");
    print_TLB_missrate();

    n_free(base, (int)bytes);

    printf("  Phase 4 completed\n\n");
}

/* --------------------------------------------------------------------------
 * Phase 5: Mixed small and large allocations with random frees
 * -------------------------------------------------------------------------- */

typedef struct {
    void *ptr;
    unsigned int size;
} alloc_rec_t;

static void phase5_mixed_fragmentation(void) {
    printf("[Phase 5] Mixed small and large allocations with random frees\n");

    alloc_rec_t records[MIX_ALLOCS];
    int live_count = 0;

    for (int i = 0; i < MIX_ALLOCS; i++) {
        records[i].ptr = NULL;
        records[i].size = 0;
    }

    srand((unsigned)time(NULL));

    for (int step = 0; step < MIX_ALLOCS; step++) {
        int make_alloc = (live_count < MIX_MAX_LIVE) ? (rand() % 2) : 0;

        if (make_alloc) {
            unsigned int sz;
            int kind = rand() % 4;
            switch (kind) {
                case 0: sz = SMALL_SZ; break;
                case 1: sz = (unsigned int)(SMALL_SZ + (rand() % 512)); break;
                case 2: sz = MEDIUM_SZ + (unsigned int)(rand() % 4096); break;
                default: sz = (unsigned int)(PGSIZE + rand() % (PGSIZE * 2)); break;
            }

            void *p = n_malloc(sz);
            if (!p) {
                printf("  step %d: n_malloc failed for sz=%u\n", step, sz);
            } else {
                int slot = -1;
                for (int j = 0; j < MIX_ALLOCS; j++) {
                    if (records[j].ptr == NULL) {
                        slot = j;
                        break;
                    }
                }
                if (slot >= 0) {
                    records[slot].ptr = p;
                    records[slot].size = sz;
                    live_count++;

                    uint32_t tag = (uint32_t)(0xC0FFEE00u ^ (uint32_t)step);
                    put_data(p, &tag, sizeof(uint32_t));
                } else {
                    n_free(p, sz);
                }
            }
        } else {
            if (live_count > 0) {
                int start = rand() % MIX_ALLOCS;
                int idx = -1;
                for (int j = 0; j < MIX_ALLOCS; j++) {
                    int k = (start + j) % MIX_ALLOCS;
                    if (records[k].ptr != NULL) {
                        idx = k;
                        break;
                    }
                }
                if (idx >= 0) {
                    void *p = records[idx].ptr;
                    unsigned int sz = records[idx].size;

                    uint32_t tag = 0;
                    get_data(p, &tag, sizeof(uint32_t));

                    n_free(p, (int)sz);

                    records[idx].ptr = NULL;
                    records[idx].size = 0;
                    live_count--;
                }
            }
        }
    }

    for (int i = 0; i < MIX_ALLOCS; i++) {
        if (records[i].ptr != NULL) {
            n_free(records[i].ptr, (int)records[i].size);
            records[i].ptr = NULL;
            records[i].size = 0;
        }
    }

    printf("  Phase 5 completed\n\n");
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(void) {
    printf("=== Heavy fragmentation + compute stress test (my_vm3) ===\n\n");

    phase0_cross_page_copy();
    phase1_heavy_mat_mult();
    phase2_multithread_mat();
    phase3_tlb_thrash();
    phase4_tlb_locality();
    phase5_mixed_fragmentation();

    printf("[Final TLB stats after heavy stress]: ");
    print_TLB_missrate();

    printf("\nAll heavy stress tests completed\n");
    return 0;
}
