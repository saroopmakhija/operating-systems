#include "my_vm64.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x20
#endif
#endif

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static void    *phys_mem64         = NULL;   // base of fake physical memory
static pde64_t *pml4               = NULL;   // level-4 page table (root)
static unsigned char *phys_bitmap  = NULL;   // 1 bit per physical page
static uint64_t       total_phys_pages = 0;

static vaddr64_t next_free_vpage   = 0;      // simple bump-pointer allocator

// TLB
static struct tlb64_entry tlb64[TLB64_ENTRIES];
static unsigned long long tlb64_lookups = 0;
static unsigned long long tlb64_misses  = 0;

// Thread safety
static pthread_mutex_t vm64_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             vm64_initialized = 0;

// ---------------------------------------------------------------------------
// Bitmap helpers (1 bit per page)
// ---------------------------------------------------------------------------

static inline void bitmap_set(unsigned char *bm, uint64_t idx) {
    bm[idx >> 3] |=  (1u << (idx & 7));
}

static inline void bitmap_clear(unsigned char *bm, uint64_t idx) {
    bm[idx >> 3] &= ~(1u << (idx & 7));
}

static inline int bitmap_get(const unsigned char *bm, uint64_t idx) {
    return (bm[idx >> 3] >> (idx & 7)) & 1u;
}

// ---------------------------------------------------------------------------
// Address component extraction
// ---------------------------------------------------------------------------

static inline uint64_t l1_index(vaddr64_t va) {
    return (va >> L1_SHIFT) & LVL_MASK;
}

static inline uint64_t l2_index(vaddr64_t va) {
    return (va >> L2_SHIFT) & LVL_MASK;
}

static inline uint64_t l3_index(vaddr64_t va) {
    return (va >> L3_SHIFT) & LVL_MASK;
}

static inline uint64_t l4_index(vaddr64_t va) {
    return (va >> L4_SHIFT) & LVL_MASK;
}

static inline uint64_t page_offset(vaddr64_t va) {
    return va & OFF64_MASK;
}

// PFN helpers
static inline uint64_t pte_get_pfn(pte64_t pte) {
    return pte >> PFN64_SHIFT;
}

static inline pte64_t pte_make(uint64_t pfn) {
    return (pfn << PFN64_SHIFT) | PTE64_VALID;
}

// Physical address -> host pointer
static inline void *paddr_to_host(paddr64_t pa) {
    return (char *)phys_mem64 + pa;
}

// PFN -> host pointer
static inline void *pfn_to_host(uint64_t pfn) {
    return (char *)phys_mem64 + (pfn * PGSIZE64);
}

// ---------------------------------------------------------------------------
// Physical page allocation
// ---------------------------------------------------------------------------

static int alloc_phys_page(uint64_t *pfn_out) {
    for (uint64_t i = 0; i < total_phys_pages; i++) {
        if (!bitmap_get(phys_bitmap, i)) {
            bitmap_set(phys_bitmap, i);
            *pfn_out = i;
            return 0;
        }
    }
    return -1; // out of physical memory
}

static void free_phys_page(uint64_t pfn) {
    if (pfn < total_phys_pages) {
        bitmap_clear(phys_bitmap, pfn);
    }
}

// ---------------------------------------------------------------------------
// TLB helpers
// ---------------------------------------------------------------------------

static int tlb64_lookup(vaddr64_t va, void **host_addr_out) {
    uint64_t vpn = va >> OFF64_BITS;
    uint64_t off = page_offset(va);
    uint64_t idx = vpn % TLB64_ENTRIES;

    tlb64_lookups++;

    struct tlb64_entry *e = &tlb64[idx];
    if (e->valid && e->vpn == vpn) {
        // hit
        uint64_t pfn = e->pfn;
        *host_addr_out = (char *)pfn_to_host(pfn) + off;
        return 1;
    }

    // miss
    tlb64_misses++;
    return 0;
}

static void tlb64_add(vaddr64_t va, uint64_t pfn) {
    uint64_t vpn = va >> OFF64_BITS;
    uint64_t idx = vpn % TLB64_ENTRIES;

    tlb64[idx].vpn   = vpn;
    tlb64[idx].pfn   = pfn;
    tlb64[idx].valid = true;
}

void vm64_print_TLB_missrate(void) {
    pthread_mutex_lock(&vm64_mutex);
    double rate = 0.0;
    if (tlb64_lookups > 0) {
        rate = (double)tlb64_misses / (double)tlb64_lookups;
    }
    printf("TLB64 miss rate %f (misses=%llu lookups=%llu)\n",
           rate,
           (unsigned long long)tlb64_misses,
           (unsigned long long)tlb64_lookups);
    pthread_mutex_unlock(&vm64_mutex);
}

// ---------------------------------------------------------------------------
// Page-table walk / creation
// ---------------------------------------------------------------------------

// Ensure that the intermediate level pointed to by *entry exists; if not
// allocate a new page table page and install it. Returns host pointer to
// the child table.
static pde64_t *ensure_pt_level(pde64_t *table, uint64_t idx) {
    pte64_t entry = table[idx];
    if (!(entry & PTE64_VALID)) {
        uint64_t new_pfn;
        if (alloc_phys_page(&new_pfn) != 0) {
            return NULL;
        }
        // zero the new page table
        void *child = pfn_to_host(new_pfn);
        memset(child, 0, PGSIZE64);
        table[idx] = pte_make(new_pfn);
        return (pde64_t *)child;
    }
    // existing: decode PFN and return
    uint64_t pfn = pte_get_pfn(entry);
    return (pde64_t *)pfn_to_host(pfn);
}

// Map a single virtual page to a physical frame (pfn).
static int map_page_single(vaddr64_t va, uint64_t pfn) {
    uint64_t i4 = l4_index(va);
    uint64_t i3 = l3_index(va);
    uint64_t i2 = l2_index(va);
    uint64_t i1 = l1_index(va);

    pde64_t *l4 = pml4;
    pde64_t *l3 = ensure_pt_level(l4, i4);
    if (!l3) return -1;
    pde64_t *l2 = ensure_pt_level(l3, i3);
    if (!l2) return -1;
    pde64_t *l1 = ensure_pt_level(l2, i2);
    if (!l1) return -1;

    // if already mapped, we simply overwrite; caller should avoid collisions
    l1[i1] = pte_make(pfn);

    // add to TLB (compulsory miss)
    tlb64_add(va, pfn);
    return 0;
}

// Translate VA -> host pointer. Returns NULL on unmapped.
static void *translate_host(vaddr64_t va) {
    // First, try TLB
    void *host;
    if (tlb64_lookup(va, &host)) {
        return host;
    }

    // Walk 4-level page table
    uint64_t i4 = l4_index(va);
    uint64_t i3 = l3_index(va);
    uint64_t i2 = l2_index(va);
    uint64_t i1 = l1_index(va);
    uint64_t off = page_offset(va);

    pde64_t *l4 = pml4;
    pte64_t e4 = l4[i4];
    if (!(e4 & PTE64_VALID)) return NULL;
    pde64_t *l3 = (pde64_t *)pfn_to_host(pte_get_pfn(e4));

    pte64_t e3 = l3[i3];
    if (!(e3 & PTE64_VALID)) return NULL;
    pde64_t *l2 = (pde64_t *)pfn_to_host(pte_get_pfn(e3));

    pte64_t e2 = l2[i2];
    if (!(e2 & PTE64_VALID)) return NULL;
    pde64_t *l1 = (pde64_t *)pfn_to_host(pte_get_pfn(e2));

    pte64_t e1 = l1[i1];
    if (!(e1 & PTE64_VALID)) return NULL;

    uint64_t pfn = pte_get_pfn(e1);
    void *base = pfn_to_host(pfn);
    host = (char *)base + off;

    // install into TLB (page-aligned)
    tlb64_add(va, pfn);
    return host;
}

// Unmap 'pages' starting at 'va'. The virtual space is not reused; we
// simply clear the PTEs and free PFNs.
static void unmap_pages(vaddr64_t va, size_t pages) {
    for (size_t k = 0; k < pages; k++) {
        vaddr64_t cur = va + (vaddr64_t)k * PGSIZE64;

        uint64_t i4 = l4_index(cur);
        uint64_t i3 = l3_index(cur);
        uint64_t i2 = l2_index(cur);
        uint64_t i1 = l1_index(cur);

        pde64_t *l4 = pml4;
        pte64_t e4 = l4[i4];
        if (!(e4 & PTE64_VALID)) continue;
        pde64_t *l3 = (pde64_t *)pfn_to_host(pte_get_pfn(e4));

        pte64_t e3 = l3[i3];
        if (!(e3 & PTE64_VALID)) continue;
        pde64_t *l2 = (pde64_t *)pfn_to_host(pte_get_pfn(e3));

        pte64_t e2 = l2[i2];
        if (!(e2 & PTE64_VALID)) continue;
        pde64_t *l1 = (pde64_t *)pfn_to_host(pte_get_pfn(e2));

        pte64_t e1 = l1[i1];
        if (!(e1 & PTE64_VALID)) continue;

        uint64_t pfn = pte_get_pfn(e1);
        free_phys_page(pfn);
        l1[i1] = 0; // clear mapping
        // Note: we do not collapse empty upper levels in this simple demo.
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void vm64_init(void) {
    pthread_mutex_lock(&vm64_mutex);
    if (vm64_initialized && phys_mem64 != NULL) {
        pthread_mutex_unlock(&vm64_mutex);
        return; // already initialized
    }

    // Allocate fake physical memory
    phys_mem64 = mmap(NULL, MEMSIZE64,
                      PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE,
                      -1, 0);
    if (phys_mem64 == MAP_FAILED) {
        perror("mmap for MEMSIZE64 failed");
        phys_mem64 = NULL;
        pthread_mutex_unlock(&vm64_mutex);
        return;
    }

    total_phys_pages = MEMSIZE64 / PGSIZE64;

    // bitmap: 1 bit per page
    uint64_t bitmap_bytes = (total_phys_pages + 7) / 8;
    phys_bitmap = (unsigned char *)calloc(bitmap_bytes, 1);
    if (!phys_bitmap) {
        fprintf(stderr, "vm64_init: failed to allocate phys_bitmap\n");
        munmap(phys_mem64, MEMSIZE64);
        phys_mem64 = NULL;
        pthread_mutex_unlock(&vm64_mutex);
        return;
    }

    // Use physical page 0 as the PML4 (root)
    bitmap_set(phys_bitmap, 0);
    pml4 = (pde64_t *)phys_mem64;
    memset(pml4, 0, PGSIZE64);

    // Initialize TLB
    for (int i = 0; i < TLB64_ENTRIES; i++) {
        tlb64[i].vpn = 0;
        tlb64[i].pfn = 0;
        tlb64[i].valid = false;
    }

    next_free_vpage = 0;
    tlb64_lookups = 0;
    tlb64_misses = 0;

    vm64_initialized = 1;

    printf("[vm64] Virtual memory initialized (4-level):\n");
    printf("  Physical memory: %llu pages (%llu MB)\n",
           (unsigned long long)total_phys_pages,
           (unsigned long long)(MEMSIZE64 / (1024 * 1024)));

    pthread_mutex_unlock(&vm64_mutex);
}

// ---------------------------------------------------------------------------
// Internal helpers that assume vm64_mutex is already held
// ---------------------------------------------------------------------------

static int vm64_put_locked(vaddr64_t va, const void *src, size_t size) {
    if (size == 0) return 0;

    const unsigned char *usrc = (const unsigned char *)src;
    size_t remaining = size;
    vaddr64_t cur_va = va;

    while (remaining > 0) {
        uint64_t off = page_offset(cur_va);
        uint64_t bytes_in_page = PGSIZE64 - off;
        if (bytes_in_page > remaining) bytes_in_page = remaining;

        void *dst_host = translate_host(cur_va);
        if (!dst_host) {
            fprintf(stderr, "vm64_put: unmapped VA 0x%llx\n",
                    (unsigned long long)cur_va);
            return -1;
        }

        memcpy((unsigned char *)dst_host, usrc, bytes_in_page);

        usrc       += bytes_in_page;
        cur_va     += bytes_in_page;
        remaining  -= bytes_in_page;
    }

    return 0;
}

static int vm64_get_locked(vaddr64_t va, void *dst, size_t size) {
    if (size == 0) return 0;

    unsigned char *udst = (unsigned char *)dst;
    size_t remaining = size;
    vaddr64_t cur_va = va;

    while (remaining > 0) {
        uint64_t off = page_offset(cur_va);
        uint64_t bytes_in_page = PGSIZE64 - off;
        if (bytes_in_page > remaining) bytes_in_page = remaining;

        void *src_host = translate_host(cur_va);
        if (!src_host) {
            fprintf(stderr, "vm64_get: unmapped VA 0x%llx\n",
                    (unsigned long long)cur_va);
            return -1;
        }

        memcpy(udst, (unsigned char *)src_host, bytes_in_page);

        udst       += bytes_in_page;
        cur_va     += bytes_in_page;
        remaining  -= bytes_in_page;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Allocation and free
// ---------------------------------------------------------------------------

vaddr64_t vm64_malloc(size_t bytes) {
    pthread_mutex_lock(&vm64_mutex);

    if (!vm64_initialized || !phys_mem64) {
        vm64_init();
        if (!vm64_initialized || !phys_mem64) {
            pthread_mutex_unlock(&vm64_mutex);
            return 0; // failure
        }
    }

    if (bytes == 0) {
        pthread_mutex_unlock(&vm64_mutex);
        return 0;
    }

    // Number of pages needed
    uint64_t pages = (bytes + PGSIZE64 - 1) / PGSIZE64;

    // Base virtual page index; bump-pointer policy
    vaddr64_t base_va = next_free_vpage * (vaddr64_t)PGSIZE64;
    next_free_vpage += pages;

    // Map each page to an available physical frame
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t pfn;
        if (alloc_phys_page(&pfn) != 0) {
            // rollback previous pages
            unmap_pages(base_va, i);
            pthread_mutex_unlock(&vm64_mutex);
            return 0;
        }
        vaddr64_t cur_va = base_va + i * (vaddr64_t)PGSIZE64;
        if (map_page_single(cur_va, pfn) != 0) {
            free_phys_page(pfn);
            unmap_pages(base_va, i);
            pthread_mutex_unlock(&vm64_mutex);
            return 0;
        }
    }

    pthread_mutex_unlock(&vm64_mutex);
    return base_va;
}

void vm64_free(vaddr64_t va, size_t bytes) {
    if (va == 0 || bytes == 0) return;

    pthread_mutex_lock(&vm64_mutex);

    size_t pages = (bytes + PGSIZE64 - 1) / PGSIZE64;
    unmap_pages(va, pages);

    pthread_mutex_unlock(&vm64_mutex);
}

// ---------------------------------------------------------------------------
// put/get (public API)
// ---------------------------------------------------------------------------

int vm64_put(vaddr64_t va, const void *src, size_t size) {
    if (size == 0) return 0;

    pthread_mutex_lock(&vm64_mutex);
    int rc = vm64_put_locked(va, src, size);
    pthread_mutex_unlock(&vm64_mutex);
    return rc;
}

int vm64_get(vaddr64_t va, void *dst, size_t size) {
    if (size == 0) return 0;

    pthread_mutex_lock(&vm64_mutex);
    int rc = vm64_get_locked(va, dst, size);
    pthread_mutex_unlock(&vm64_mutex);
    return rc;
}

// ---------------------------------------------------------------------------
// Matrix multiply (int matrices)
// ---------------------------------------------------------------------------

void vm64_mat_mult(vaddr64_t mat1, vaddr64_t mat2, int n, vaddr64_t out) {
    if (n <= 0) return;

    pthread_mutex_lock(&vm64_mutex);

    int *rowA = (int *)malloc((size_t)n * sizeof(int));
    int *colB = (int *)malloc((size_t)n * sizeof(int));
    if (!rowA || !colB) {
        fprintf(stderr, "vm64_mat_mult: malloc failed\n");
        free(rowA);
        free(colB);
        pthread_mutex_unlock(&vm64_mutex);
        return;
    }

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            // Load row i of A
            for (int k = 0; k < n; k++) {
                vaddr64_t vaA = mat1 +
                    (vaddr64_t)i * (vaddr64_t)n * sizeof(int) +
                    (vaddr64_t)k * sizeof(int);
                if (vm64_get_locked(vaA, &rowA[k], sizeof(int)) != 0) {
                    fprintf(stderr, "vm64_mat_mult: read A failed\n");
                }
            }
            // Load column j of B
            for (int k = 0; k < n; k++) {
                vaddr64_t vaB = mat2 +
                    (vaddr64_t)k * (vaddr64_t)n * sizeof(int) +
                    (vaddr64_t)j * sizeof(int);
                if (vm64_get_locked(vaB, &colB[k], sizeof(int)) != 0) {
                    fprintf(stderr, "vm64_mat_mult: read B failed\n");
                }
            }

            long long sum = 0;
            for (int k = 0; k < n; k++) {
                sum += (long long)rowA[k] * (long long)colB[k];
            }
            int result = (int)sum;

            vaddr64_t vaC = out +
                (vaddr64_t)i * (vaddr64_t)n * sizeof(int) +
                (vaddr64_t)j * sizeof(int);
            if (vm64_put_locked(vaC, &result, sizeof(int)) != 0) {
                fprintf(stderr, "vm64_mat_mult: write C failed\n");
            }
        }
    }

    free(rowA);
    free(colB);

    pthread_mutex_unlock(&vm64_mutex);
}
