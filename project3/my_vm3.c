// ssm229 Saroop Makhija
// mk2177 Aiman Koli 
#include "my_vm3.h"
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

// Global state

static void *physical_memory   = NULL;   // our simulated physical RAM base
static pde_t *page_directory   = NULL;   // top-level page directory
static unsigned char *physical_bitmap = NULL;
static unsigned char *virtual_bitmap  = NULL;

vpage_meta_t *vpage_meta       = NULL;

static bool is_initialized     = false;

static pthread_mutex_t vm_mutex;
static pthread_mutex_t tlb_mutex;

static pthread_once_t vm_init_once = PTHREAD_ONCE_INIT;

static uint32_t total_physical_pages;
static uint32_t total_virtual_pages;

struct tlb tlb_store[TLB_ENTRIES];

static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses  = 0;

// Internal helper functions

static int   bitmap_find_free(unsigned char *bitmap, uint32_t num_bits);
static void *alloc_physical_page(void);
static void *get_physical_addr(void *va);

// Extra credit helpers for fragmentation reduction
static void  free_full_vpage(uint32_t vpage_num);
static int   find_small_page_with_space(unsigned int num_bytes);
static int   alloc_small_page(void);

// Bitmap helpers

void bitmap_set(unsigned char *bitmap, int page_index) {
    bitmap[page_index / 8] |= (1 << (page_index % 8));
}

void bitmap_clear(unsigned char *bitmap, int page_index) {
    bitmap[page_index / 8] &= ~(1 << (page_index % 8));
}

int bitmap_get(unsigned char *bitmap, int page_index) {
    return (bitmap[page_index / 8] >> (page_index % 8)) & 1;
}

static int bitmap_find_free(unsigned char *bitmap, uint32_t num_bits) {
    for (uint32_t i = 0; i < num_bits; i++) {
        if (!bitmap_get(bitmap, (int)i)) {
            return (int)i;
        }
    }
    return -1;
}

// Initialization

void set_physical_mem(void) {
    if (is_initialized) {
        return;
    }

    if (pthread_mutex_init(&vm_mutex, NULL) != 0 ||
        pthread_mutex_init(&tlb_mutex, NULL) != 0) {
        fprintf(stderr, "set_physical_mem: failed to init mutexes\n");
        exit(EXIT_FAILURE);
    }

    total_physical_pages = MEMSIZE     / PGSIZE;
    total_virtual_pages  = MAX_MEMSIZE / PGSIZE;

    uint32_t physical_bitmap_size = (total_physical_pages + 7u) / 8u;
    uint32_t virtual_bitmap_size  = (total_virtual_pages  + 7u) / 8u;

    physical_memory = mmap(NULL, MEMSIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE,
                           -1, 0);
    if (physical_memory == MAP_FAILED) {
        fprintf(stderr, "set_physical_mem: failed to allocate physical memory\n");
        exit(EXIT_FAILURE);
    }

    physical_bitmap = calloc(physical_bitmap_size, 1);
    virtual_bitmap  = calloc(virtual_bitmap_size, 1);
    vpage_meta      = calloc(total_virtual_pages, sizeof(vpage_meta_t));

    if (!physical_bitmap || !virtual_bitmap || !vpage_meta) {
        fprintf(stderr, "set_physical_mem: failed to allocate bitmaps/metadata\n");
        exit(EXIT_FAILURE);
    }

    // Put the page directory in the first page of physical memory
    page_directory = (pde_t *)physical_memory;
    memset(page_directory, 0, PGSIZE);

    // Mark first frame as used (holds the page directory)
    bitmap_set(physical_bitmap, 0);

    // Reserve virtual page 0 so NULL is never valid
    bitmap_set(virtual_bitmap, 0);
    vpage_meta[0].kind = 1;

    // Clear TLB and reset stats
    memset(tlb_store, 0, sizeof(tlb_store));
    tlb_lookups = 0;
    tlb_misses  = 0;

    is_initialized = true;

    printf("Virtual memory initialized:\n");
    printf("  Physical memory: %u pages (%llu MB)\n",
           total_physical_pages,
           (unsigned long long)(MEMSIZE / (1024ULL * 1024ULL)));
    printf("  Virtual memory: %u pages (%llu MB)\n",
           total_virtual_pages,
           (unsigned long long)(MAX_MEMSIZE / (1024ULL * 1024ULL)));
}

// TLB

int TLB_add(void *va, void *pa) {
    if (!va || !pa || !is_initialized) {
        return -1;
    }

    vaddr32_t vaddr = VA2U(va);
    paddr32_t paddr = (paddr32_t)((uintptr_t)pa - (uintptr_t)physical_memory);

    uint32_t vpn   = vaddr >> OFFSET_BITS;
    uint32_t pfn   = paddr >> OFFSET_BITS;
    uint32_t index = vpn % TLB_ENTRIES;

    pthread_mutex_lock(&tlb_mutex);
    tlb_store[index].vpn   = vpn;
    tlb_store[index].pfn   = pfn;
    tlb_store[index].valid = true;
    pthread_mutex_unlock(&tlb_mutex);

    return 0;
}

void *TLB_check(void *va) {
    if (!va || !is_initialized) {
        return NULL;
    }

    vaddr32_t vaddr = VA2U(va);
    uint32_t vpn    = vaddr >> OFFSET_BITS;
    uint32_t index  = vpn % TLB_ENTRIES;

    pthread_mutex_lock(&tlb_mutex);
    tlb_lookups++;

    if (tlb_store[index].valid && tlb_store[index].vpn == vpn) {
        uint32_t pfn    = tlb_store[index].pfn;
        uint32_t offset = OFF(va);
        void *pa        = (char *)physical_memory + (pfn * PGSIZE) + offset;
        pthread_mutex_unlock(&tlb_mutex);
        return pa;
    }

    tlb_misses++;
    pthread_mutex_unlock(&tlb_mutex);
    return NULL;
}

void print_TLB_missrate(void) {
    pthread_mutex_lock(&tlb_mutex);
    unsigned long long lookups = tlb_lookups;
    unsigned long long misses  = tlb_misses;
    pthread_mutex_unlock(&tlb_mutex);

    double miss_rate = 0.0;
    if (lookups > 0) {
        miss_rate = (double)misses / (double)lookups;
    }
    printf("TLB miss rate %lf\n", miss_rate);
}

// Page table operations

// Caller is responsible for holding vm_mutex if needed
pte_t *translate(pde_t *pgdir, void *va) {
    if (!pgdir || !va) {
        return NULL;
    }

    uint32_t pdx = PDX(va);
    uint32_t ptx = PTX(va);

    pde_t pde = pgdir[pdx];
    if (!(pde & PTE_VALID)) {
        return NULL;
    }

    uint32_t pt_pfn   = pde >> PFN_SHIFT;
    pte_t *page_table = (pte_t *)((char *)physical_memory + pt_pfn * PGSIZE);

    pte_t *pte_ptr = &page_table[ptx];
    if (!(*pte_ptr & PTE_VALID)) {
        return NULL;
    }

    return pte_ptr;
}

// Caller must hold vm_mutex
int map_page(pde_t *pgdir, void *va, void *pa) {
    if (!pgdir || !va || !pa) {
        return -1;
    }

    paddr32_t paddr = (paddr32_t)((uintptr_t)pa - (uintptr_t)physical_memory);

    uint32_t pd_index = PDX(va);
    uint32_t pt_index = PTX(va);

    pde_t *pde_ptr    = &pgdir[pd_index];
    pte_t *page_table = NULL;

    if (*pde_ptr == 0) {
        page_table = (pte_t *)alloc_physical_page();
        if (!page_table) {
            perror("map_page: failed to allocate page table");
            return -1;
        }

        memset(page_table, 0, PGSIZE);

        paddr32_t pt_offset = (paddr32_t)((uintptr_t)page_table -
                                          (uintptr_t)physical_memory);
        uint32_t  pt_pfn    = pt_offset >> OFFSET_BITS;

        *pde_ptr = (pt_pfn << PFN_SHIFT) | PTE_VALID;
    } else {
        uint32_t pt_pfn = *pde_ptr >> PFN_SHIFT;
        page_table = (pte_t *)((char *)physical_memory + pt_pfn * PGSIZE);
    }

    uint32_t data_pfn = paddr >> OFFSET_BITS;
    page_table[pt_index] = (data_pfn << PFN_SHIFT) | PTE_VALID;

    return 0;
}

// Physical page allocation

// Caller must hold vm_mutex
static void *alloc_physical_page(void) {
    int page_num = bitmap_find_free(physical_bitmap, total_physical_pages);
    if (page_num < 0) {
        fprintf(stderr, "Error: Out of physical memory\n");
        return NULL;
    }

    bitmap_set(physical_bitmap, page_num);
    return (char *)physical_memory + (page_num * PGSIZE);
}

// Virtual page allocation (bitmap)

// Caller must hold vm_mutex
void *get_next_avail(int num_pages) {
    for (uint32_t i = 0; i + (uint32_t)num_pages <= total_virtual_pages; i++) {
        bool found = true;

        for (int j = 0; j < num_pages; j++) {
            if (bitmap_get(virtual_bitmap, (int)(i + (uint32_t)j))) {
                found = false;
                break;
            }
        }

        if (found) {
            for (int j = 0; j < num_pages; j++) {
                bitmap_set(virtual_bitmap, (int)(i + (uint32_t)j));
            }

            vaddr32_t va = i * PGSIZE;
            return U2VA(va);
        }
    }

    return NULL;
}

// Extra credit helpers for fragmentation

// Caller must hold vm_mutex
static void free_full_vpage(uint32_t vpage_num) {
    if (vpage_num >= total_virtual_pages) {
        return;
    }

    vaddr32_t page_va = vpage_num * PGSIZE;

    pte_t *pte = translate(page_directory, U2VA(page_va));
    if (pte && (*pte & PTE_VALID)) {
        uint32_t pfn = *pte >> PFN_SHIFT;
        bitmap_clear(physical_bitmap, (int)pfn);
        *pte = 0;
    }

    bitmap_clear(virtual_bitmap, (int)vpage_num);

    if (vpage_meta) {
        vpage_meta[vpage_num].kind             = 0;
        vpage_meta[vpage_num].active_suballocs = 0;
        vpage_meta[vpage_num].next_free_off    = 0;
    }

    // Invalidate any TLB entry for this VPN
    pthread_mutex_lock(&tlb_mutex);
    uint32_t vpn = vpage_num;
    for (int i = 0; i < TLB_ENTRIES; i++) {
        if (tlb_store[i].valid && tlb_store[i].vpn == vpn) {
            tlb_store[i].valid = false;
        }
    }
    pthread_mutex_unlock(&tlb_mutex);
}

// Caller must hold vm_mutex
static int find_small_page_with_space(unsigned int num_bytes) {
    for (uint32_t vp = 1; vp < total_virtual_pages; vp++) {
        if (vpage_meta[vp].kind == 2) {
            uint32_t used = vpage_meta[vp].next_free_off;
            if (used + num_bytes <= PGSIZE) {
                return (int)vp;
            }
        }
    }
    return -1;
}

// Caller must hold vm_mutex
static int alloc_small_page(void) {
    void *va_base = get_next_avail(1);
    if (!va_base) {
        fprintf(stderr, "alloc_small_page: no free virtual pages\n");
        return -1;
    }

    void *pa = alloc_physical_page();
    if (!pa) {
        fprintf(stderr, "alloc_small_page: out of physical memory\n");
        uint32_t vpage = VA2U(va_base) / PGSIZE;
        bitmap_clear(virtual_bitmap, (int)vpage);
        return -1;
    }

    memset(pa, 0, PGSIZE);

    if (map_page(page_directory, va_base, pa) < 0) {
        fprintf(stderr, "alloc_small_page: map_page failed\n");
        uint32_t vpage = VA2U(va_base) / PGSIZE;
        bitmap_clear(virtual_bitmap, (int)vpage);

        uint32_t pfn = (uint32_t)(((char *)pa - (char *)physical_memory) /
                                  PGSIZE);
        bitmap_clear(physical_bitmap, (int)pfn);
        return -1;
    }

    uint32_t vp = VA2U(va_base) / PGSIZE;
    vpage_meta[vp].kind             = 2;
    vpage_meta[vp].active_suballocs = 0;
    vpage_meta[vp].next_free_off    = 0;

    return (int)vp;
}

// n_malloc / n_free with fragmentation reduction

void *n_malloc(unsigned int num_bytes) {
    pthread_once(&vm_init_once, set_physical_mem);

    if (!is_initialized || num_bytes == 0) {
        return NULL;
    }

    pthread_mutex_lock(&vm_mutex);

    // Small allocation path: pack many into one page
    if (num_bytes < PGSIZE) {
        int vp = find_small_page_with_space(num_bytes);
        if (vp < 0) {
            vp = alloc_small_page();
            if (vp < 0) {
                pthread_mutex_unlock(&vm_mutex);
                return NULL;
            }
        }

        uint32_t offset = vpage_meta[vp].next_free_off;
        vpage_meta[vp].next_free_off    += num_bytes;
        vpage_meta[vp].active_suballocs += 1;

        vaddr32_t result_va = (vaddr32_t)vp * PGSIZE + offset;
        pthread_mutex_unlock(&vm_mutex);
        return U2VA(result_va);
    }

    // Large allocation path: allocate whole pages, contiguous virtually
    int num_pages = (int)((num_bytes + PGSIZE - 1u) / PGSIZE);

    void *va_base = get_next_avail(num_pages);
    if (!va_base) {
        fprintf(stderr, "Error: Cannot find %d contiguous virtual pages\n",
                num_pages);
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }

    vaddr32_t base_u      = VA2U(va_base);
    uint32_t  start_vpage = base_u / PGSIZE;
    int       mapped_pages = 0;

    for (int i = 0; i < num_pages; i++) {
        vaddr32_t page_va_u = base_u + (vaddr32_t)i * PGSIZE;
        void     *page_va   = U2VA(page_va_u);

        void *pa = alloc_physical_page();
        if (!pa) {
            fprintf(stderr, "Error: Failed to allocate physical page %d/%d\n",
                    i + 1, num_pages);

            // Roll back mapped pages
            for (int j = 0; j < mapped_pages; j++) {
                vaddr32_t cleanup_va_u = base_u + (vaddr32_t)j * PGSIZE;
                void     *cleanup_va   = U2VA(cleanup_va_u);

                pte_t *pte = translate(page_directory, cleanup_va);
                if (pte && (*pte & PTE_VALID)) {
                    uint32_t pfn = *pte >> PFN_SHIFT;
                    bitmap_clear(physical_bitmap, (int)pfn);
                    *pte = 0;
                }
            }

            // Clear virtual bits and metadata
            for (int j = 0; j < num_pages; j++) {
                bitmap_clear(virtual_bitmap,
                             (int)(start_vpage + (uint32_t)j));
                if (vpage_meta) {
                    vpage_meta[start_vpage + (uint32_t)j].kind             = 0;
                    vpage_meta[start_vpage + (uint32_t)j].active_suballocs = 0;
                    vpage_meta[start_vpage + (uint32_t)j].next_free_off    = 0;
                }
            }

            pthread_mutex_unlock(&vm_mutex);
            return NULL;
        }

        memset(pa, 0, PGSIZE);

        if (map_page(page_directory, page_va, pa) < 0) {
            fprintf(stderr, "Error: Failed to map page %d/%d\n",
                    i + 1, num_pages);

            paddr32_t pa_off = (paddr32_t)((uintptr_t)pa -
                                           (uintptr_t)physical_memory);
            uint32_t  pfn_new = pa_off >> OFFSET_BITS;
            bitmap_clear(physical_bitmap, (int)pfn_new);

            for (int j = 0; j < mapped_pages; j++) {
                vaddr32_t cleanup_va_u = base_u + (vaddr32_t)j * PGSIZE;
                void     *cleanup_va   = U2VA(cleanup_va_u);

                pte_t *pte = translate(page_directory, cleanup_va);
                if (pte && (*pte & PTE_VALID)) {
                    uint32_t pfn = *pte >> PFN_SHIFT;
                    bitmap_clear(physical_bitmap, (int)pfn);
                    *pte = 0;
                }
            }

            for (int j = 0; j < num_pages; j++) {
                bitmap_clear(virtual_bitmap,
                             (int)(start_vpage + (uint32_t)j));
                if (vpage_meta) {
                    vpage_meta[start_vpage + (uint32_t)j].kind             = 0;
                    vpage_meta[start_vpage + (uint32_t)j].active_suballocs = 0;
                    vpage_meta[start_vpage + (uint32_t)j].next_free_off    = 0;
                }
            }

            pthread_mutex_unlock(&vm_mutex);
            return NULL;
        }

        uint32_t vpage_num = page_va_u / PGSIZE;
        if (vpage_meta) {
            vpage_meta[vpage_num].kind             = 1;
            vpage_meta[vpage_num].active_suballocs = 0;
            vpage_meta[vpage_num].next_free_off    = PGSIZE;
        }

        mapped_pages++;
    }

    pthread_mutex_unlock(&vm_mutex);
    return va_base;
}

void n_free(void *va, int size) {
    if (!va || size <= 0 || !is_initialized) {
        fprintf(stderr, "Error: Invalid n_free parameters\n");
        return;
    }

    vaddr32_t vaddr     = VA2U(va);
    uint32_t  vpage_num = vaddr / PGSIZE;

    pthread_mutex_lock(&vm_mutex);

    if (vpage_num >= total_virtual_pages) {
        fprintf(stderr, "Warning: n_free on out of range VA %p\n", va);
        pthread_mutex_unlock(&vm_mutex);
        return;
    }

    // Small allocation free
    if (size < (int)PGSIZE &&
        vpage_meta &&
        vpage_meta[vpage_num].kind == 2) {

        if (vpage_meta[vpage_num].active_suballocs == 0) {
            fprintf(stderr,
                    "Warning: freeing small allocation from empty page at %p\n",
                    va);
        } else {
            vpage_meta[vpage_num].active_suballocs--;
        }

        if (vpage_meta[vpage_num].active_suballocs == 0) {
            free_full_vpage(vpage_num);
        }

        pthread_mutex_unlock(&vm_mutex);
        return;
    }

    // Large allocation free: free all pages that region covers
    int       num_pages  = (int)((size + PGSIZE - 1u) / PGSIZE);
    vaddr32_t vaddr_base = vaddr - (vaddr % PGSIZE);

    for (int i = 0; i < num_pages; i++) {
        vaddr32_t current_va = vaddr_base + (vaddr32_t)i * PGSIZE;
        uint32_t  vp         = current_va / PGSIZE;

        if (vp >= total_virtual_pages) {
            fprintf(stderr,
                    "Warning: n_free large free out-of-range page at %p\n",
                    U2VA(current_va));
            continue;
        }

        if (!bitmap_get(virtual_bitmap, (int)vp)) {
            fprintf(stderr,
                    "Warning: Trying to free unallocated virtual page at %p\n",
                    U2VA(current_va));
            continue;
        }

        free_full_vpage(vp);
    }

    pthread_mutex_unlock(&vm_mutex);
}

// Address translation helper used by put_data / get_data

static void *get_physical_addr(void *va) {
    if (!is_initialized || !va) {
        return NULL;
    }

    void *pa = TLB_check(va);
    if (pa) {
        return pa;
    }

    uint32_t pdx    = PDX(va);
    uint32_t ptx    = PTX(va);
    uint32_t offset = OFF(va);

    pthread_mutex_lock(&vm_mutex);

    pde_t pde = page_directory[pdx];
    if (!(pde & PTE_VALID)) {
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }

    uint32_t pt_pfn   = pde >> PFN_SHIFT;
    pte_t *page_table = (pte_t *)((char *)physical_memory + pt_pfn * PGSIZE);

    pte_t pte = page_table[ptx];
    if (!(pte & PTE_VALID)) {
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }

    uint32_t data_pfn = pte >> PFN_SHIFT;
    void *page_base   = (char *)physical_memory + data_pfn * PGSIZE;

    pthread_mutex_unlock(&vm_mutex);

    TLB_add(va, page_base);

    return (char *)page_base + offset;
}

// Data movement

int put_data(void *va, void *val, int size) {
    if (!va || !val || size <= 0 || !is_initialized) {
        fprintf(stderr, "Error: Invalid put_data parameters\n");
        return -1;
    }

    vaddr32_t current_va = VA2U(va);
    int       remaining  = size;
    int       src_offset = 0;

    while (remaining > 0) {
        void *pa = get_physical_addr(U2VA(current_va));
        if (!pa) {
            fprintf(stderr,
                    "Error: Virtual address %p not mapped in put_data\n",
                    U2VA(current_va));
            return -1;
        }

        uint32_t page_offset   = OFF(U2VA(current_va));
        uint32_t bytes_in_page = PGSIZE - page_offset;
        if (bytes_in_page > (uint32_t)remaining) {
            bytes_in_page = (uint32_t)remaining;
        }

        memcpy(pa, (char *)val + src_offset, bytes_in_page);

        remaining  -= (int)bytes_in_page;
        src_offset += (int)bytes_in_page;
        current_va += bytes_in_page;
    }

    return 0;
}

void get_data(void *va, void *val, int size) {
    if (!va || !val || size <= 0 || !is_initialized) {
        fprintf(stderr, "Error: Invalid get_data parameters\n");
        return;
    }

    vaddr32_t current_va = VA2U(va);
    int       remaining  = size;
    int       dst_offset = 0;

    while (remaining > 0) {
        void *pa = get_physical_addr(U2VA(current_va));
        if (!pa) {
            fprintf(stderr,
                    "Error: Virtual address %p not mapped in get_data\n",
                    U2VA(current_va));
            return;
        }

        uint32_t page_offset   = OFF(U2VA(current_va));
        uint32_t bytes_in_page = PGSIZE - page_offset;
        if (bytes_in_page > (uint32_t)remaining) {
            bytes_in_page = (uint32_t)remaining;
        }

        memcpy((char *)val + dst_offset, pa, bytes_in_page);

        remaining  -= (int)bytes_in_page;
        dst_offset += (int)bytes_in_page;
        current_va += bytes_in_page;
    }
}

// Matrix multiplication

void mat_mult(void *mat1, void *mat2, int size, void *answer) {
    if (!mat1 || !mat2 || !answer || size <= 0) {
        fprintf(stderr, "Error: Invalid mat_mult parameters\n");
        return;
    }

    printf("Starting matrix multiplication: %dx%d matrices\n", size, size);

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            uint32_t sum = 0;

            for (int k = 0; k < size; k++) {
                vaddr32_t addr_mat1 = VA2U(mat1)
                                     + ((vaddr32_t)(i * size + k)
                                        * (vaddr32_t)sizeof(uint32_t));
                vaddr32_t addr_mat2 = VA2U(mat2)
                                     + ((vaddr32_t)(k * size + j)
                                        * (vaddr32_t)sizeof(uint32_t));

                uint32_t a, b;

                get_data(U2VA(addr_mat1), &a, sizeof(uint32_t));
                get_data(U2VA(addr_mat2), &b, sizeof(uint32_t));

                sum += a * b;
            }

            vaddr32_t addr_answer = VA2U(answer)
                                   + ((vaddr32_t)(i * size + j)
                                      * (vaddr32_t)sizeof(uint32_t));

            put_data(U2VA(addr_answer), &sum, sizeof(uint32_t));
        }

        if ((i + 1) % 10 == 0) {
            printf("  Completed row %d/%d\n", i + 1, size);
        }
    }

    printf("Matrix multiplication complete\n");
}
