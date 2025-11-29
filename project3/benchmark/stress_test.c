// fault_test.c
// Negative / edge-case tests for 32-bit VM library (my_vm.c / my_vm.h)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "../my_vm.h"

// small helper for safe VA + offset
static inline void *va_add(void *base, size_t off) {
    vaddr32_t u = VA2U(base);
    u += (uint32_t)off;
    return U2VA(u);
}

// ------------------------------------------------------
// Phase 0: init + double init
// ------------------------------------------------------
static void phase0_init(void) {
    printf("\n[Phase 0] set_physical_mem() and double-initialization\n");

    printf("  First call to set_physical_mem()...\n");
    set_physical_mem();

    printf("  Second call to set_physical_mem() (should warn, not crash)...\n");
    set_physical_mem();

    printf("  Phase 0 completed\n");
}

// ------------------------------------------------------
// Phase 1: n_malloc edge sizes (0, huge) and simple free
// ------------------------------------------------------
static void phase1_alloc_sizes(void) {
    printf("\n[Phase 1] n_malloc() edge sizes (0, huge)\n");

    // 1) n_malloc(0)
    void *p0 = n_malloc(0);
    if (p0 == NULL) {
        printf("  n_malloc(0) returned NULL (acceptable)\n");
    } else {
        printf("  n_malloc(0) returned VA=%#x (also acceptable, but unusual)\n",
               VA2U(p0));
        // Try freeing it to ensure we don't crash
        n_free(p0, 0);
    }

    // 2) Oversized allocation (bigger than physical memory)
    //    This should fail cleanly (NULL or error), not crash.
    unsigned int huge_size = (unsigned int)(MEMSIZE * 4); // 4GB for 1GB physical
    void *p_big = n_malloc(huge_size);
    if (!p_big) {
        printf("  n_malloc(huge=%u) failed as expected\n", huge_size);
    } else {
        printf("  [WARN] n_malloc(huge=%u) unexpectedly succeeded, VA=%#x\n",
               huge_size, VA2U(p_big));
        n_free(p_big, huge_size);
    }

    printf("  Phase 1 completed\n");
}

// ------------------------------------------------------
// Phase 2: out-of-range put_data/get_data
// ------------------------------------------------------
static void phase2_out_of_range(void) {
    printf("\n[Phase 2] Out-of-range put_data/get_data\n");

    // Allocate exactly one page
    unsigned int bytes = PGSIZE;
    void *base = n_malloc(bytes);
    if (!base) {
        printf("  Phase 2 aborted: n_malloc(PGSIZE) failed\n");
        return;
    }
    printf("  Allocated one page at VA=%#x\n", VA2U(base));

    int val = 0x12345678;
    int out = 0;

    // 1) Completely outside mapped region: base + PGSIZE
    void *va_unmapped = va_add(base, PGSIZE);
    printf("  put_data() at VA just beyond end (should error)...\n");
    int rc1 = put_data(va_unmapped, &val, sizeof(val));
    printf("    put_data returned %d\n", rc1);

    // 2) Cross-page write: start near end of page, size crosses into unmapped page
    printf("  put_data() that crosses page boundary (should error or partially fail)...\n");
    void *va_cross = va_add(base, PGSIZE - sizeof(int) / 2);
    int rc2 = put_data(va_cross, &val, sizeof(val));
    printf("    put_data returned %d\n", rc2);

    // 3) get_data() from unmapped region
    printf("  get_data() from unmapped VA (should error)...\n");
    int rc3 = 0;
    rc3 = get_data == NULL; // avoid unused-warning if get_data is void-return
    (void)rc3;              // we rely on your implementation printing errors

    // Actual call: result printed by your library
    get_data(va_unmapped, &out, sizeof(out));

    // 4) get_data() crossing page boundary
    get_data(va_cross, &out, sizeof(out));

    n_free(base, bytes);
    printf("  Phase 2 completed\n");
}

// ------------------------------------------------------
// Phase 3: invalid frees, double frees, partial and overlapping frees
// ------------------------------------------------------
static void phase3_free_errors(void) {
    printf("\n[Phase 3] n_free() error cases\n");

    // Allocate two regions
    unsigned int sz1 = PGSIZE * 2;      // 2 pages
    unsigned int sz2 = PGSIZE * 3;      // 3 pages

    void *p1 = n_malloc(sz1);
    void *p2 = n_malloc(sz2);

    if (!p1 || !p2) {
        printf("  Phase 3 aborted: allocations failed (p1=%p p2=%p)\n", p1, p2);
        if (p1) n_free(p1, sz1);
        if (p2) n_free(p2, sz2);
        return;
    }

    printf("  p1=%#x (size=%u), p2=%#x (size=%u)\n",
           VA2U(p1), sz1, VA2U(p2), sz2);

    // 1) Double free of same range
    printf("  Double free of p1 (should warn but not crash)...\n");
    n_free(p1, sz1);
    n_free(p1, sz1);  // second free should trigger warning

    // 2) Free pointer that was never allocated
    void *fake = U2VA(0xDEADBEEF);
    printf("  Freeing completely fake VA=%#x (should warn)...\n", VA2U(fake));
    n_free(fake, PGSIZE);

    // 3) Free partially overlapping region: inside p2 range but not at page boundary
    void *mid = va_add(p2, PGSIZE / 2);  // middle of first page of p2
    printf("  Freeing overlapping region starting mid-page of p2 (behavior: error or free that page)...\n");
    n_free(mid, PGSIZE);

    // 4) Free beyond where p2 ends
    void *beyond = va_add(p2, sz2);
    printf("  Freeing region beyond p2 (unallocated pages, should error)...\n");
    n_free(beyond, PGSIZE);

    // Try to free p2 "normally" (might partially succeed/fail depending on 3/4)
    printf("  Freeing p2 original range...\n");
    n_free(p2, sz2);

    printf("  Phase 3 completed\n");
}

// ------------------------------------------------------
// Phase 4: write/read across unmapped address ranges
// (using arbitrary VAs not returned by n_malloc)
// ------------------------------------------------------
static void phase4_arbitrary_VA(void) {
    printf("\n[Phase 4] put_data/get_data on arbitrary unmapped VAs\n");

    int val = 0xCAFEBABE;
    int out = 0;

    // A few arbitrary addresses that should not be mapped
    vaddr32_t bad_addrs[] = {
        0x0,          // NULL page (you reserved virtual page 0)
        0x2000,       // likely unmapped early on
        0x40000000,   // mid of 4GB VA space
        0x80000000,   // higher range
        0xFFFFF000    // last page in VA space
    };

    for (size_t i = 0; i < sizeof(bad_addrs)/sizeof(bad_addrs[0]); i++) {
        void *va = U2VA(bad_addrs[i]);
        printf("  put_data to unmapped VA=%#x...\n", bad_addrs[i]);
        int rc = put_data(va, &val, sizeof(val));
        printf("    put_data returned %d\n", rc);

        printf("  get_data from unmapped VA=%#x...\n", bad_addrs[i]);
        get_data(va, &out, sizeof(out)); // we rely on your library to print error
    }

    printf("  Phase 4 completed\n");
}

// ------------------------------------------------------
// Phase 5: mat_mult with under-allocated matrix (forces invalid accesses)
// ------------------------------------------------------
static void phase5_bad_matmult(void) {
    printf("\n[Phase 5] mat_mult() reading from unmapped memory\n");
    printf("  Note: this phase is EXPECTED to print many \"Virtual address not mapped\" errors.\n");
    printf("        We only care that it doesn't segfault.\n");

    // Choose n such that full matrix size > PGSIZE, but we only allocate ~half that.
    // For PGSIZE=4096 and sizeof(int)=4, n=40 => matrix size = 40*40*4 = 6400 bytes.
    // We'll allocate ~3200 bytes -> only 1 page mapped, but mat_mult will touch >1 page.
    int n = 40;
    size_t full_bytes = (size_t)n * n * sizeof(int);
    size_t under_bytes = full_bytes / 2;

    // Allocate A under-sized; B and C fully sized.
    void *A = n_malloc((unsigned int)under_bytes);
    void *B = n_malloc((unsigned int)full_bytes);
    void *C = n_malloc((unsigned int)full_bytes);

    if (!A || !B || !C) {
        printf("  Phase 5 aborted: allocations failed (A=%p B=%p C=%p)\n", A, B, C);
        if (A) n_free(A, (int)under_bytes);
        if (B) n_free(B, (int)full_bytes);
        if (C) n_free(C, (int)full_bytes);
        return;
    }

    printf("  A (under-sized) VA=%#x size=%zu\n", VA2U(A), under_bytes);
    printf("  B, C (full) VA=%#x, %#x size=%zu\n", VA2U(B), VA2U(C), full_bytes);

    int *hostA = (int *)malloc(full_bytes);
    int *hostB = (int *)malloc(full_bytes);
    if (!hostA || !hostB) {
        printf("  Phase 5 host malloc failed\n");
        free(hostA);
        free(hostB);
        n_free(A, (int)under_bytes);
        n_free(B, (int)full_bytes);
        n_free(C, (int)full_bytes);
        return;
    }

    // Fill with small values
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            hostA[i*n + j] = 1;           // A all ones
            hostB[i*n + j] = (i == j);    // B = identity
        }
    }

    // Put only as many bytes as we allocated for A
    put_data(A, hostA, (int)under_bytes);
    put_data(B, hostB, (int)full_bytes);

    printf("  Calling mat_mult(A_under, B_full, n, C_full)...\n");
    printf("  Expected behavior: your get_data/put_data should complain and return errors,\n");
    printf("  but the process MUST NOT segfault.\n");

    mat_mult(A, B, n, C);

    // Clean up
    n_free(A, (int)under_bytes);
    n_free(B, (int)full_bytes);
    n_free(C, (int)full_bytes);
    free(hostA);
    free(hostB);

    printf("  Phase 5 completed\n");
}

// ------------------------------------------------------
// main
// ------------------------------------------------------
int main(void) {
    srand((unsigned)time(NULL));

    printf("=== 32-bit VM Negative / Edge-Case Test ===\n");

    // Phase 0 also calls set_physical_mem() and double-calls it
    phase0_init();

    // Remaining phases assume physical memory already initialized
    phase1_alloc_sizes();
    phase2_out_of_range();
    phase3_free_errors();
    phase4_arbitrary_VA();
    phase5_bad_matmult();

    printf("\nAll negative/edge-case tests completed (check above for warnings/errors).\n");
    return 0;
}
