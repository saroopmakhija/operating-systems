// Fixing internal fragmentation optmising malloc


#include "my_vm3.h"
#include <string.h>   // optional for memcpy if you later implement put/get
#include <sys/mman.h>
#include <pthread.h>

// -----------------------------------------------------------------------------
// Global Declarations (optional)
// -----------------------------------------------------------------------------

static void *physical_memory = NULL;      // Simulated physical RAM
static pde_t *page_directory = NULL;      // Top-level page directory
static unsigned char *physical_bitmap = NULL;  // Track physical pages
static unsigned char *virtual_bitmap = NULL;   // Track virtual pages
static bool is_initialized = false;
static pthread_mutex_t vm_mutex;
static pthread_mutexattr_t vm_mutex_attr;
static pthread_once_t vm_init_once = PTHREAD_ONCE_INIT;

// Memory configuration
static uint32_t total_physical_pages;
static uint32_t total_virtual_pages;
static uint32_t bitmap_size;

struct tlb tlb_store[TLB_ENTRIES]; // Placeholder for your TLB structure

// Optional counters for TLB statistics
static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses  = 0;

// Fragmentation reduction helpers 
static void free_full_vpage(uint32_t vpage_num);
static int  find_small_page_with_space(unsigned int num_bytes);
static int  alloc_small_page(void);

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------


void bitmap_set(unsigned char *bitmap, int page_index) {
    bitmap[page_index / 8] |= (1 << (page_index % 8));
    //first get the byte of the index
    //then get the bit in that with remainder
    //bit shift by the remainder and 'or add' to byte
    //edits byte in place
}

void bitmap_clear(unsigned char *bitmap, int page_index) {
    bitmap[page_index / 8] &= ~(1 << (page_index % 8));
}

int bitmap_get(unsigned char *bitmap, int page_index) {
    return (bitmap[page_index / 8] >> (page_index % 8)) & 1;
}

// Find first free bit, return index or -1
static int bitmap_find_free(unsigned char *bitmap, uint32_t num_bits) {
    for (uint32_t i = 0; i < num_bits; i++) {
        if (!bitmap_get(bitmap, i)) {
            return i;
        }
    }
    return -1;
}

// Forward declaration
static void *alloc_physical_page(void);

/*
 * set_physical_mem()
 * ------------------
 * Allocates and initializes simulated physical memory and any required
 * data structures (e.g., bitmaps for tracking page use).
 *
 * Return value: None.
 * Errors should be handled internally (e.g., failed allocation).
 */

void set_physical_mem(void) {
    // TODO: Implement memory allocation for simulated physical memory.
    // Use 32-bit values for sizes, page counts, and offsets.
    
    // Initialize recursive mutex on first call
    // if (!is_initialized) {
    //     pthread_mutexattr_init(&vm_mutex_attr);
    //     pthread_mutexattr_settype(&vm_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    //     pthread_mutex_init(&vm_mutex, &vm_mutex_attr);
    // }

    pthread_mutexattr_init(&vm_mutex_attr);
    pthread_mutexattr_settype(&vm_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&vm_mutex, &vm_mutex_attr);
    
    // pthread_mutex_lock(&vm_mutex);
    // if (is_initialized) {
    //     fprintf(stderr, "physical memory already initialized\n");
    //     pthread_mutex_unlock(&vm_mutex);
    //     return;
    // }
    // Calculate page counts
    total_physical_pages = MEMSIZE / PGSIZE;  // 1GB / 4KB = 262,144 pages
    total_virtual_pages = MAX_MEMSIZE / PGSIZE;  // 4GB / 4KB = 1,048,576 pages

    uint32_t physical_bitmap_size = (total_physical_pages + 7) / 8;
    uint32_t virtual_bitmap_size = (total_virtual_pages + 7) / 8;

    physical_memory = mmap(NULL, MEMSIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (physical_memory == MAP_FAILED) {
        fprintf(stderr, "failed to allocate physical memory\n");
        return;
    }

    physical_bitmap = calloc(physical_bitmap_size, 1);
    virtual_bitmap  = calloc(virtual_bitmap_size, 1);
    vpage_meta      = calloc(total_virtual_pages, sizeof(vpage_meta_t));

    if (!physical_bitmap || !virtual_bitmap || !vpage_meta) {
        fprintf(stderr, "failed to allocate memory for physical memory metadata\n");
        return;
    }

    page_directory = (pde_t *)physical_memory;  // put page directory at the start of physical mem
    memset(physical_memory, 0, PGSIZE);         // the page directory is 4KB

    // first physical page holds the page directory, mark it used
    bitmap_set(physical_bitmap, 0);

    // reserve virtual page 0 (so we never return NULL as a valid address)
    bitmap_set(virtual_bitmap, 0);
    vpage_meta[0].kind = 1;   // treat vpage 0 as permanently reserved


    is_initialized = true;
    // pthread_mutex_unlock(&vm_mutex);

    printf("Virtual memory initialized:\n");
    printf("  Physical memory: %u pages (%llu MB)\n", 
           total_physical_pages, MEMSIZE / (1024*1024));
    printf("  Virtual memory: %u pages (%llu MB)\n", 
           total_virtual_pages, MAX_MEMSIZE / (1024*1024));

}

// -----------------------------------------------------------------------------
// TLB
// -----------------------------------------------------------------------------

/*
 * TLB_add()
 * ---------
 * Adds a new virtual-to-physical translation to the TLB.
 * Ensure thread safety when updating shared TLB data.
 *
 * Return:
 *   0  -> Success (translation successfully added)
 *  -1  -> Failure (e.g., TLB full or invalid input)
 */
 int TLB_add(void *va, void *pa)
 {
     if (!va || !pa || !is_initialized) {
         return -1;
     }
 
     vaddr32_t vaddr = VA2U(va);
     // physical offset from start of simulated RAM
     paddr32_t paddr = (paddr32_t)((uintptr_t)pa - (uintptr_t)physical_memory);
 
     uint32_t vpn   = vaddr >> OFFSET_BITS;      // virtual page number
     uint32_t pfn   = paddr >> OFFSET_BITS;      // physical frame number
     uint32_t index = vpn % TLB_ENTRIES;         // direct-mapped index
 
     pthread_mutex_lock(&vm_mutex);
 
     tlb_store[index].vpn   = vpn;
     tlb_store[index].pfn   = pfn;
     tlb_store[index].valid = true;
 
     pthread_mutex_unlock(&vm_mutex);
 
     return 0;
 }


 // get entire physical address page base + offset
 static void *get_physical_addr(void *va){
    if (!is_initialized || !va) return NULL;

    void *pa = TLB_check(va);  

    if (pa) return pa;
    
    vaddr32_t vaddr = VA2U(va);
    uint32_t pdx    = PDX(va);
    uint32_t ptx    = PTX(va);
    uint32_t offset = OFF(va);

    pthread_mutex_lock(&vm_mutex);

    pde_t pde = page_directory[pdx];
    if (!(pde & PTE_VALID)) {
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }

    uint32_t pt_pfn = pde >> PFN_SHIFT;  
    pte_t *page_table = (pte_t *)((char *)physical_memory + pt_pfn * PGSIZE);

    pte_t pte = page_table[ptx];
    if (!(pte & PTE_VALID)) {
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }
    uint32_t data_pfn  = pte >> PFN_SHIFT;
    void *page_base    = (char *)physical_memory + data_pfn * PGSIZE;

    TLB_add(va, page_base);

    pthread_mutex_unlock(&vm_mutex);

    return (char *)page_base + offset; 

 }
 
/*
 * TLB_check()
 * -----------
 * Looks up a virtual address in the TLB.
 *
 * Return:
 *   Pointer to the corresponding page table entry (PTE) if found.
 *   NULL if the translation is not found (TLB miss).
 */
void *TLB_check(void *va)
{
 
    if (!va || !is_initialized) {
        return NULL;
    }

    vaddr32_t vaddr = VA2U(va);
    uint32_t vpn = vaddr >> OFFSET_BITS;
    uint32_t index = vpn % TLB_ENTRIES;

    pthread_mutex_lock(&vm_mutex);
    tlb_lookups++;

    if (tlb_store[index].valid && tlb_store[index].vpn == vpn) {
        uint32_t pfn = tlb_store[index].pfn;
        uint32_t offset = OFF(va);
        void *pa = (char *)physical_memory + (pfn * PGSIZE) + offset;
        pthread_mutex_unlock(&vm_mutex);
        return pa;
    }

    // TLB MISS
    tlb_misses++;
    pthread_mutex_unlock(&vm_mutex);
    return NULL;

}

/*
 * print_TLB_missrate()
 * --------------------
 * Calculates and prints the TLB miss rate.
 *
 * Return value: None.
 */
 void print_TLB_missrate(void)
 {
     double miss_rate = 0.0;
     if (tlb_lookups > 0) {
         miss_rate = (double)tlb_misses / (double)tlb_lookups;
     }
     printf("TLB miss rate %lf\n", miss_rate);
 }
 

// -----------------------------------------------------------------------------
// Page Table
// -----------------------------------------------------------------------------

/*
 * translate()
 * -----------
 * Translates a virtual address to a physical address.
 * Perform a TLB lookup first; if not found, walk the page directory
 * and page tables using a two-level lookup.
 *
 * Return:
 *   Pointer to the PTE structure if translation succeeds.
 *   NULL if translation fails (e.g., page not mapped).
 */
pte_t *translate(pde_t *pgdir, void *va)
{
    // TODO: Extract the 32-bit virtual address and compute indices
    // for the page directory, page table, and offset.
    // Return the corresponding PTE if found.

    //saroop add the cahce stuff here - this is without cache implementation

    if (!pgdir || !va) {
        fprintf(stderr, "translate: pgdir or va is NULL\n");
        return NULL;
    }

    int pdx = PDX(va);
    int ptx = PTX(va);
    //int off = OFF(va);

    pthread_mutex_lock(&vm_mutex);
    pde_t pde = pgdir[pdx]; // get the page directory entry
    if (!pde || !(pde & PTE_VALID) || pde==0) {
        fprintf(stderr, "translate: page directory entry not found\n");
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }
    
    
    uintptr_t pt_pfn = pde >> PFNSHIFT;
    pte_t *page_table =(pte_t *)((char *)physical_memory + pt_pfn * PGSIZE);
    pte_t *pte_ptr = &page_table[ptx];
    
    // Check if page is mapped
    if (*pte_ptr == 0 || !(*pte_ptr & PTE_VALID)) {
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }
    
    pthread_mutex_unlock(&vm_mutex);
    return pte_ptr;
}

/*
 * map_page()
 * -----------
 * Establishes a mapping between a virtual and a physical page.
 * Creates intermediate page tables if necessary.
 *
 * Return:
 *   0  -> Success (mapping created)
 *  -1  -> Failure (e.g., no space or invalid address)
 */
 int map_page(pde_t *pgdir, void *va, void *pa) {
    if (!pgdir || !va || !pa) {
        return -1;
    }
    
    // convert
    vaddr32_t vaddr = VA2U(va);
    paddr32_t paddr = (paddr32_t)(uintptr_t)pa - (paddr32_t)(uintptr_t)physical_memory;
    
    // Extract page directory index and page table index
    uint32_t pd_index = PDX(va);
    uint32_t pt_index = PTX(va);
    
    pthread_mutex_lock(&vm_mutex);
    
    // Get pointer to page directory entry
    pde_t *pde_ptr = &pgdir[pd_index];
    pte_t *page_table = NULL;
    
    // Check if page table exists
    if (*pde_ptr == 0) {
        // Allocate a new page table
        page_table = (pte_t *)alloc_physical_page();
        if (!page_table) {
            pthread_mutex_unlock(&vm_mutex);
            perror("map_page: failed to allocate physical page");
            return -1;
        }
        
        // Initialize page table to all zeros
        memset(page_table, 0, PGSIZE);
        
        // Store page table address in page directory entry
        // Convert physical address to offset from physical_memory base
        paddr32_t pt_offset = (paddr32_t)((uintptr_t)page_table - (uintptr_t)physical_memory);
        uint32_t pfn = pt_offset >> OFFSET_BITS;
        *pde_ptr = (pfn << PFN_SHIFT) | PTE_VALID;
    } else {
        // Page table exists, retrieve it
        uint32_t pfn = *pde_ptr >> PFN_SHIFT;
        page_table = (pte_t *)((char *)physical_memory + pfn * PGSIZE);
    }
    
    // Get physical frame number
    uint32_t pfn = paddr >> OFFSET_BITS;
    
    // Set page table entry
    page_table[pt_index] = (pfn << PFN_SHIFT) | PTE_VALID;
    
    pthread_mutex_unlock(&vm_mutex);
    return 0;
}


// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------

/*
 * Helper: Allocate a physical page
 */
static void *alloc_physical_page(void) {
    int page_num = bitmap_find_free(physical_bitmap, total_physical_pages);
    
    if (page_num < 0) {
        fprintf(stderr, "Error: Out of physical memory\n");
        return NULL;
    }
    
    bitmap_set(physical_bitmap, page_num);
    return (char *)physical_memory + (page_num * PGSIZE);
}

/*
 * get_next_avail()
 * ----------------
 * Finds and returns the base virtual address of the next available
 * block of contiguous free pages.
 *
 * Return:
 *   Pointer to the base virtual address if available.
 *   NULL if there are no sufficient free pages.
 */
void *get_next_avail(int num_pages)
{
    // Search for num_pages consecutive free pages
    for (uint32_t i = 0; i <= total_virtual_pages - num_pages; i++) {
        bool found = true;
        
        // Check if num_pages starting at i are all free
        for (int j = 0; j < num_pages; j++) {
            if (bitmap_get(virtual_bitmap, i + j)) {
                found = false;
                break;
            }
        }
        
        if (found) {
            // Mark these pages as allocated
            for (int j = 0; j < num_pages; j++) {
                bitmap_set(virtual_bitmap, i + j);
            }
            
            // Convert page number to virtual address
            vaddr32_t va = i * PGSIZE;
            return U2VA(va);
        }
    }
    
    return NULL;  // No space available
}

/*
 * n_malloc()
 * -----------
 * Allocates a given number of bytes in virtual memory.
 * Initializes physical memory and page directories if not already done.
 *
 * Return:
 *   Pointer to the starting virtual address of allocated memory (success).
 *   NULL if allocation fails.
 */
 void *n_malloc(unsigned int num_bytes)
 {
     // Initialize on first call
     pthread_once(&vm_init_once, set_physical_mem);
 
     if (num_bytes == 0 || !is_initialized) {
         return NULL;
     }
 
     pthread_mutex_lock(&vm_mutex);
 
     // -----------------------------------------------------------------
     // Small allocation path: pack multiple allocations into one page
     // -----------------------------------------------------------------
     if (num_bytes < PGSIZE) {
         int vp = find_small_page_with_space(num_bytes);
 
         if (vp < 0) {
             // Need a new page to act as a small-allocation pool
             vp = alloc_small_page();
             if (vp < 0) {
                 pthread_mutex_unlock(&vm_mutex);
                 return NULL;
             }
         }
 
         // Allocate from this small-allocation page using a simple bump pointer
         uint32_t offset = vpage_meta[vp].next_free_off;
         vpage_meta[vp].next_free_off    += num_bytes;
         vpage_meta[vp].active_suballocs += 1;
 
         vaddr32_t result_va = vp * PGSIZE + offset;
         pthread_mutex_unlock(&vm_mutex);
         return U2VA(result_va);
     }
 
     // -----------------------------------------------------------------
     // Large allocation path: same as your original page-based scheme
     // -----------------------------------------------------------------
     int num_pages = (num_bytes + PGSIZE - 1) / PGSIZE;
 
     // Find contiguous virtual pages
     void *va_base = get_next_avail(num_pages);
     if (!va_base) {
         fprintf(stderr, "Error: Cannot find %d contiguous virtual pages\n", num_pages);
         pthread_mutex_unlock(&vm_mutex);
         return NULL;
     }
 
     vaddr32_t base_va_u32 = VA2U(va_base);
 
     // Allocate physical pages and create mappings
     for (int i = 0; i < num_pages; i++) {
         vaddr32_t va_page = base_va_u32 + (i * PGSIZE);
         void *pa = alloc_physical_page();
         if (!pa) {
             fprintf(stderr, "Error: Failed to allocate physical page %d/%d\n", i + 1, num_pages);
 
             // Clean up virtual pages we already reserved
             for (int j = 0; j < num_pages; j++) {
                 vaddr32_t cleanup_va  = base_va_u32 + (j * PGSIZE);
                 uint32_t vpage_num    = cleanup_va / PGSIZE;
 
                 if (bitmap_get(virtual_bitmap, vpage_num)) {
                     bitmap_clear(virtual_bitmap, vpage_num);
                     if (vpage_meta) {
                         vpage_meta[vpage_num].kind             = 0;
                         vpage_meta[vpage_num].active_suballocs = 0;
                         vpage_meta[vpage_num].next_free_off    = 0;
                     }
                 }
             }
 
             pthread_mutex_unlock(&vm_mutex);
             return NULL;
         }
 
         memset(pa, 0, PGSIZE);
 
         if (map_page(page_directory, U2VA(va_page), pa) < 0) {
             fprintf(stderr, "Error: Failed to map page %d/%d\n", i + 1, num_pages);
             pthread_mutex_unlock(&vm_mutex);
             return NULL;
         }
 
         // Mark metadata as a large allocation page
         uint32_t vpage_num = va_page / PGSIZE;
         vpage_meta[vpage_num].kind             = 1;
         vpage_meta[vpage_num].active_suballocs = 0;
         vpage_meta[vpage_num].next_free_off    = PGSIZE;
     }
 
     pthread_mutex_unlock(&vm_mutex);
     return va_base;
 }
 


// Free a full virtual page: clear PTE, physical and virtual bitmaps, and metadata.
// Caller must hold vm_mutex.
static void free_full_vpage(uint32_t vpage_num) {
    if (vpage_num >= total_virtual_pages) return;

    vaddr32_t page_va = vpage_num * PGSIZE;

    // Clear page-table entry and physical bitmap
    pte_t *pte = translate(page_directory, U2VA(page_va));
    if (pte) {
        uint32_t pfn = *pte >> PFN_SHIFT;
        bitmap_clear(physical_bitmap, pfn);
        *pte = 0;
    }

    // Clear virtual bitmap and metadata
    bitmap_clear(virtual_bitmap, vpage_num);
    vpage_meta[vpage_num].kind            = 0;
    vpage_meta[vpage_num].active_suballocs = 0;
    vpage_meta[vpage_num].next_free_off    = 0;
}

// Find an existing small-allocation page with enough free space.
// Caller must hold vm_mutex.
static int find_small_page_with_space(unsigned int num_bytes) {
    for (uint32_t vp = 1; vp < total_virtual_pages; vp++) {
        if (vpage_meta[vp].kind == 2) {
            uint32_t used      = vpage_meta[vp].next_free_off;
            if (used + num_bytes <= PGSIZE) {
                return (int)vp;
            }
        }
    }
    return -1;
}

// Allocate a new virtual page and map one physical page for small allocations.
// Returns vpage index on success, -1 on failure.
// Caller must hold vm_mutex.
static int alloc_small_page(void) {
    void *va_base = get_next_avail(1);  // reserves 1 virtual page in the bitmap
    if (!va_base) {
        fprintf(stderr, "alloc_small_page: no free virtual pages\n");
        return -1;
    }

    // Allocate physical page
    void *pa = alloc_physical_page();
    if (!pa) {
        fprintf(stderr, "alloc_small_page: out of physical memory\n");
        uint32_t vpage = VA2U(va_base) / PGSIZE;
        bitmap_clear(virtual_bitmap, vpage);
        return -1;
    }

    memset(pa, 0, PGSIZE);

    if (map_page(page_directory, va_base, pa) < 0) {
        fprintf(stderr, "alloc_small_page: map_page failed\n");
        uint32_t vpage = VA2U(va_base) / PGSIZE;
        bitmap_clear(virtual_bitmap, vpage);

        // free physical page in bitmap
        uint32_t pfn = ((char *)pa - (char *)physical_memory) / PGSIZE;
        bitmap_clear(physical_bitmap, pfn);
        return -1;
    }

    uint32_t vp = VA2U(va_base) / PGSIZE;
    vpage_meta[vp].kind             = 2;
    vpage_meta[vp].active_suballocs = 0;
    vpage_meta[vp].next_free_off    = 0;

    return (int)vp;
}


/*
 * n_free()
 * ---------
 * Frees one or more pages of memory starting at the given virtual address.
 * Marks the corresponding virtual and physical pages as free.
 * Removes the translation from the TLB.
 *
 * Return value: None.
 */
 void n_free(void *va, int size)
 {
     if (!va || size <= 0 || !is_initialized) {
         fprintf(stderr, "Error: Invalid n_free parameters\n");
         return;
     }
 
     vaddr32_t vaddr      = VA2U(va);
     uint32_t  vpage_num  = vaddr / PGSIZE;
 
     pthread_mutex_lock(&vm_mutex);
 
     if (vpage_num >= total_virtual_pages) {
         fprintf(stderr, "Warning: n_free on out-of-range virtual address %p\n", va);
         pthread_mutex_unlock(&vm_mutex);
         return;
     }
 
     // -----------------------------------------------------------------
     // Small allocation free (inside a small-allocation page)
     // -----------------------------------------------------------------
     if (size < (int)PGSIZE && vpage_meta[vpage_num].kind == 2) {
         if (vpage_meta[vpage_num].active_suballocs == 0) {
             fprintf(stderr, "Warning: freeing small allocation from empty page at %p\n", va);
         } else {
             vpage_meta[vpage_num].active_suballocs--;
         }
 
         // Only when all suballocations are gone do we release the page
         if (vpage_meta[vpage_num].active_suballocs == 0) {
             free_full_vpage(vpage_num);
         }
 
         pthread_mutex_unlock(&vm_mutex);
         return;
     }
 
     // -----------------------------------------------------------------
     // Large allocation free (page-based), same semantics as before
     // -----------------------------------------------------------------
     int num_pages = (size + PGSIZE - 1) / PGSIZE;
     vaddr32_t vaddr_base = vaddr - (vaddr % PGSIZE);   // align down to page boundary
 
     for (int i = 0; i < num_pages; i++) {
         vaddr32_t current_va = vaddr_base + (i * PGSIZE);
         uint32_t  vp         = current_va / PGSIZE;
 
         if (vp >= total_virtual_pages) {
             fprintf(stderr, "Warning: n_free large free out-of-range page at %p\n",
                     U2VA(current_va));
             continue;
         }
 
         // Check if virtual page is actually allocated
         if (!bitmap_get(virtual_bitmap, vp)) {
             fprintf(stderr, "Warning: Trying to free unallocated virtual page at %p\n",
                     U2VA(current_va));
             continue;
         }
 
         // Free the full page (PTE + physical + bitmap + metadata)
         free_full_vpage(vp);
     }
 
     pthread_mutex_unlock(&vm_mutex);
 }
 
// -----------------------------------------------------------------------------
// Data Movement
// -----------------------------------------------------------------------------

/*
 * put_data()
 * ----------
 * Copies data from a user buffer into simulated physical memory using
 * the virtual address. Handle page boundaries properly.
 *
 * Return:
 *   0  -> Success (data written successfully)
 *  -1  -> Failure (e.g., translation failure)
 */
int put_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0 || !is_initialized) {
        fprintf(stderr, "Error: Invalid put_data parameters\n");
        return -1;
    }
    
    vaddr32_t current_va = VA2U(va);
    int remaining = size;
    int src_offset = 0;
    
    pthread_mutex_lock(&vm_mutex);
    
    while (remaining > 0) {
        void *pa = get_physical_addr(U2VA(current_va));
        // Translate virtual address to get PTE
        // pte_t *pte = translate(page_directory, U2VA(current_va));
        
        if (!pa) {
            fprintf(stderr, "Error: Virtual address %p not mapped\n", U2VA(current_va));
            pthread_mutex_unlock(&vm_mutex);
            return -1;
        }
        
        // // Get physical frame number
        // uint32_t pfn = *pte >> PFN_SHIFT;
        
        // Calculate offset within the page
        uint32_t page_offset = OFF(U2VA(current_va));
        
        // Calculate how many bytes to copy in this page
        uint32_t bytes_in_page = PGSIZE - page_offset;
        if (bytes_in_page > remaining) {
            bytes_in_page = remaining;
        }
        
        // Calculate physical address
        // void *physical_addr = (char *)physical_memory + (pfn * PGSIZE) + page_offset;
        
        // Copy data
        memcpy(pa, (char *)val + src_offset, bytes_in_page);
        
        // Update counters
        remaining -= bytes_in_page;
        src_offset += bytes_in_page;
        current_va += bytes_in_page;
    }
    
    pthread_mutex_unlock(&vm_mutex);
    return 0;
}

/*
 * get_data()
 * -----------
 * Copies data from simulated physical memory (accessed via virtual address)
 * into a user buffer.
 *
 * Return value: None.
 */
void get_data(void *va, void *val, int size)
{
    if (!va || !val || size <= 0 || !is_initialized) {
        fprintf(stderr, "Error: Invalid get_data parameters\n");
        return;
    }
    
    vaddr32_t current_va = VA2U(va);
    int remaining = size;
    int dst_offset = 0;
    
    pthread_mutex_lock(&vm_mutex);
    
    while (remaining > 0) {
        // // Translate virtual address to get PTE
        // pte_t *pte = translate(page_directory, U2VA(current_va));
        void *pa = get_physical_addr(U2VA(current_va));

        if (!pa) {
            fprintf(stderr, "Error: Virtual address %p not mapped\n", U2VA(current_va));
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        
        // Get physical frame number
        // uint32_t pfn = *pte >> PFN_SHIFT;
        
        // Calculate offset within the page
        uint32_t page_offset = OFF(U2VA(current_va));
        
        // Calculate how many bytes to copy in this page
        uint32_t bytes_in_page = PGSIZE - page_offset;
        if (bytes_in_page > remaining) {
            bytes_in_page = remaining;
        }
        
        // Calculate physical address
        // void *physical_addr = (char *)physical_memory + (pfn * PGSIZE) + page_offset;
        
        // Copy data
        memcpy((char *)val + dst_offset, pa, bytes_in_page);
        
        // Update counters
        remaining -= bytes_in_page;
        dst_offset += bytes_in_page;
        current_va += bytes_in_page;
    }
    
    pthread_mutex_unlock(&vm_mutex);
}

// -----------------------------------------------------------------------------
// Matrix Multiplication
// -----------------------------------------------------------------------------

/*
 * mat_mult()
 * ----------
 * Performs matrix multiplication of two matrices stored in virtual memory.
 * Each element is accessed and stored using get_data() and put_data().
 *
 * Return value: None.
 */
void mat_mult(void *mat1, void *mat2, int size, void *answer)
{
    if (!mat1 || !mat2 || !answer || size <= 0) {
        fprintf(stderr, "Error: Invalid mat_mult parameters\n");
        return;
    }
    
    printf("Starting matrix multiplication: %dx%d matrices\n", size, size);
    
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            uint32_t sum = 0;
            
            for (int k = 0; k < size; k++) {
                // Calculate addresses for mat1[i][k] and mat2[k][j]
                // Using row-major order: A[i][j] = A + (i * size + j) * sizeof(element)
                vaddr32_t addr_mat1 = VA2U(mat1) + ((i * size + k) * sizeof(uint32_t));
                vaddr32_t addr_mat2 = VA2U(mat2) + ((k * size + j) * sizeof(uint32_t));
                
                uint32_t a, b;
                
                // Read mat1[i][k]
                get_data(U2VA(addr_mat1), &a, sizeof(uint32_t));
                
                // Read mat2[k][j]
                get_data(U2VA(addr_mat2), &b, sizeof(uint32_t));
                
                // Accumulate
                sum += (a * b);
            }
            
            // Calculate address for answer[i][j]
            vaddr32_t addr_answer = VA2U(answer) + ((i * size + j) * sizeof(uint32_t));
            
            // Write result
            put_data(U2VA(addr_answer), &sum, sizeof(uint32_t));
        }
        
        // Progress indicator for large matrices
        if ((i + 1) % 10 == 0) {
            printf("  Completed row %d/%d\n", i + 1, size);
        }
    }
    
    printf("Matrix multiplication complete\n");
}

