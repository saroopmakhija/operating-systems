#include "../my_vm3.h"
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUM_SMALL       16
#define SMALL_ALLOC_SZ  (PGSIZE / 16)   /* definitely < PGSIZE / 4 */
#define MAX_PACK_ALLOCS 1024
#define NTHREADS        8
#define ALLOCS_PER_THREAD 256

static void phase0_basic_packing(void) {
    printf("[Phase 0] Basic small-allocation packing in one page\n");

    void *ptrs[NUM_SMALL];
    for (int i = 0; i < NUM_SMALL; i++) {
        ptrs[i] = n_malloc(SMALL_ALLOC_SZ);
        if (!ptrs[i]) {
            printf("  n_malloc failed at i=%d\n", i);
            return;
        }
    }

    uint32_t first_page = VA2U(ptrs[0]) / PGSIZE;
    int all_same = 1;

    for (int i = 0; i < NUM_SMALL; i++) {
        uint32_t vp = VA2U(ptrs[i]) / PGSIZE;
        printf("  ptr[%2d] = %p  vpage=0x%08x  offset=0x%04x\n",
               i, ptrs[i], vp, (unsigned)(VA2U(ptrs[i]) % PGSIZE));
        if (vp != first_page) {
            all_same = 0;
        }

        int val = 1000 + i;
        if (put_data(ptrs[i], &val, sizeof(int)) != 0) {
            printf("    put_data failed at i=%d\n", i);
        }
    }

    for (int i = 0; i < NUM_SMALL; i++) {
        int read_back = 0;
        get_data(ptrs[i], &read_back, sizeof(int));
        printf("    read_back[%2d] = %d\n", i, read_back);
    }

    if (all_same) {
        printf("  Result: all %d small allocations are in the SAME virtual page (0x%08x)\n",
               NUM_SMALL, first_page);
    } else {
        printf("  Result: allocations spread across multiple pages, fragmentation higher\n");
    }

    for (int i = 0; i < NUM_SMALL; i++) {
        n_free(ptrs[i], SMALL_ALLOC_SZ);
    }

    printf("  Phase 0 completed\n\n");
}

static void phase1_page_cross(void) {
    printf("[Phase 1] How many small allocations fit in one virtual page\n");

    void *ptrs[MAX_PACK_ALLOCS];
    for (int i = 0; i < MAX_PACK_ALLOCS; i++) {
        ptrs[i] = NULL;
    }

    ptrs[0] = n_malloc(SMALL_ALLOC_SZ);
    if (!ptrs[0]) {
        printf("  First allocation failed, cannot test packing\n");
        return;
    }

    uint32_t first_page = VA2U(ptrs[0]) / PGSIZE;
    int used = 1;
    int crossed = 0;

    for (int i = 1; i < MAX_PACK_ALLOCS; i++) {
        ptrs[i] = n_malloc(SMALL_ALLOC_SZ);
        if (!ptrs[i]) {
            printf("  Out of space at i=%d before seeing a new page\n", i);
            used = i;
            break;
        }
        uint32_t vp = VA2U(ptrs[i]) / PGSIZE;
        if (vp != first_page) {
            printf("  First new virtual page after %d allocations: old=0x%08x new=0x%08x\n",
                   i, first_page, vp);
            used = i + 1;
            crossed = 1;
            break;
        }
    }

    if (!crossed) {
        printf("  Did not observe a second page within %d small allocations\n",
               MAX_PACK_ALLOCS);
    }

    for (int i = 0; i < used; i++) {
        if (ptrs[i]) {
            n_free(ptrs[i], SMALL_ALLOC_SZ);
        }
    }

    printf("  Phase 1 completed\n\n");
}

static void phase2_free_reuse(void) {
    printf("[Phase 2] Freeing all small allocations from a page and checking reuse\n");

    void *ptrs[NUM_SMALL];
    for (int i = 0; i < NUM_SMALL; i++) {
        ptrs[i] = n_malloc(SMALL_ALLOC_SZ);
        if (!ptrs[i]) {
            printf("  n_malloc failed at i=%d\n", i);
            return;
        }
    }

    uint32_t vpage0 = VA2U(ptrs[0]) / PGSIZE;
    printf("  Small allocations vpage0 = 0x%08x\n", vpage0);

    for (int i = 0; i < NUM_SMALL; i++) {
        n_free(ptrs[i], SMALL_ALLOC_SZ);
    }

    void *big = n_malloc(PGSIZE);
    if (!big) {
        printf("  Large allocation after freeing smalls failed\n");
        return;
    }

    uint32_t big_vpage = VA2U(big) / PGSIZE;
    printf("  Big allocation VA=%p vpage=0x%08x\n", big, big_vpage);

    if (big_vpage == vpage0) {
        printf("  Result: reclaimed the freed small-allocation page for a big allocation\n");
    } else {
        printf("  Result: big allocation did NOT reuse that freed page (allocator policy)\n");
    }

    n_free(big, PGSIZE);

    printf("  Phase 2 completed\n\n");
}

typedef struct {
    int tid;
} thread_arg_t;

static void *small_alloc_thread(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    int tid = t->tid;

    void *ptrs[ALLOCS_PER_THREAD];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        ptrs[i] = n_malloc(SMALL_ALLOC_SZ);
        if (!ptrs[i]) {
            printf("  [thread %d] n_malloc failed at i=%d\n", tid, i);
            break;
        }

        int value = tid * 1000 + i;
        if (put_data(ptrs[i], &value, sizeof(int)) != 0) {
            printf("  [thread %d] put_data failed at i=%d\n", tid, i);
        }
    }

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        if (!ptrs[i]) {
            continue;
        }
        int value = 0;
        get_data(ptrs[i], &value, sizeof(int));
        int expected = tid * 1000 + i;
        if (value != expected) {
            printf("  [thread %d] data mismatch at i=%d: got %d expected %d\n",
                   tid, i, value, expected);
        }
    }

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        if (ptrs[i]) {
            n_free(ptrs[i], SMALL_ALLOC_SZ);
        }
    }

    return NULL;
}

static void phase3_multithread_small(void) {
    printf("[Phase 3] Multithreaded small allocations with fragmentation-aware allocator\n");

    pthread_t threads[NTHREADS];
    thread_arg_t args[NTHREADS];

    for (int i = 0; i < NTHREADS; i++) {
        args[i].tid = i;
        if (pthread_create(&threads[i], NULL, small_alloc_thread, &args[i]) != 0) {
            perror("  pthread_create");
            exit(1);
        }
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Phase 3 completed\n\n");
}

int main(void) {
    printf("=== Fragmentation-aware allocator test (my_vm3) ===\n\n");

    srand((unsigned)time(NULL));

    phase0_basic_packing();
    phase1_page_cross();
    phase2_free_reuse();
    phase3_multithread_small();

    printf("[Summary] TLB statistics after fragmentation test:\n");
    print_TLB_missrate();

    printf("\nAll fragmentation tests completed\n");
    return 0;
}
