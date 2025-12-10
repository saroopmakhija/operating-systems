// ssm229 Saroop Makhija
// mk2177 Aiman Koli 
#include "my_vm.h"
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

// -----------------------------------------------------------------------------
// Global Declarations (optional)
// -----------------------------------------------------------------------------

static void *physical_memory = NULL;   // Our simulated physical RAM
static pde_t *page_directory = NULL;   // Top-level page directory
static unsigned char *physical_bitmap = NULL;
static unsigned char *virtual_bitmap  = NULL;

static bool is_initialized = false;


static pthread_mutex_t vm_mutex;
static pthread_mutex_t tlb_mutex;

static pthread_once_t vm_init_once = PTHREAD_ONCE_INIT;

// Memory configuration
static uint32_t total_physical_pages;
static uint32_t total_virtual_pages;

struct tlb tlb_store[TLB_ENTRIES];

static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses  = 0;
// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------


void bitmap_set(unsigned char *bitmap, int page_index) {
    bitmap[page_index / 8] |= (1 << (page_index % 8));
    // Find the right byte, then the bit within it
    // Shift and OR it in to mark as used
}

void bitmap_clear(unsigned char *bitmap, int page_index) {
    bitmap[page_index / 8] &= ~(1 << (page_index % 8));
}

int bitmap_get(unsigned char *bitmap, int page_index) {
    return (bitmap[page_index / 8] >> (page_index % 8)) & 1;
}

// Finds first free bit in bitmap, returns index or -1 if none found
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

 void set_physical_mem(void)
 {
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
 
     uint32_t physical_bitmap_size = (total_physical_pages + 7) / 8;
     uint32_t virtual_bitmap_size  = (total_virtual_pages  + 7) / 8;
 
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
     if (!physical_bitmap || !virtual_bitmap) {
         fprintf(stderr, "set_physical_mem: failed to allocate bitmaps\n");
         exit(EXIT_FAILURE);
     }
 
    // Put the page directory in the first page of physical memory
    page_directory = (pde_t *)physical_memory;
    memset(page_directory, 0, PGSIZE);

    // Mark first physical frame as used (it holds the page directory)
    bitmap_set(physical_bitmap, 0);

    // Reserve virtual page 0 so NULL never becomes a valid address
    bitmap_set(virtual_bitmap, 0);

    // Clear TLB and reset stats
     memset(tlb_store, 0, sizeof(tlb_store));
     tlb_lookups = 0;
     tlb_misses  = 0;
 
     is_initialized = true;
 
     printf("Virtual memory initialized:\n");
     printf("  Physical memory: %u pages (%llu MB)\n",
            total_physical_pages, MEMSIZE / (1024ULL * 1024ULL));
     printf("  Virtual memory: %u pages (%llu MB)\n",
            total_virtual_pages, MAX_MEMSIZE / (1024ULL * 1024ULL));
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
    // Calculate offset from start of our simulated RAM
    paddr32_t paddr = (paddr32_t)((uintptr_t)pa - (uintptr_t)physical_memory);

    uint32_t vpn = vaddr >> OFFSET_BITS;
    uint32_t pfn = paddr >> OFFSET_BITS;
    uint32_t index = vpn % TLB_ENTRIES;   // using direct-mapped TLB
 
    pthread_mutex_lock(&tlb_mutex);
 
    tlb_store[index].vpn   = vpn;
    tlb_store[index].pfn   = pfn;
    tlb_store[index].valid = true;
 
    pthread_mutex_unlock(&tlb_mutex);
 
    return 0;
 }
 

 // Gets the full physical address (page base + offset) for a virtual address
 static void *get_physical_addr(void *va)
 {
     if (!is_initialized || !va) {
         return NULL;
     }

     // Try TLB first
     void *pa = TLB_check(va);
     if (pa) {
         return pa;   // Got it from TLB
     }
 
     vaddr32_t vaddr = VA2U(va);
     uint32_t pdx = PDX(va);
     uint32_t ptx = PTX(va);
     uint32_t offset = OFF(va);
 
     pthread_mutex_lock(&vm_mutex);
 
     pde_t pde = page_directory[pdx];
     if (!(pde & PTE_VALID)) {
         pthread_mutex_unlock(&vm_mutex);
         return NULL;
     }
 
     uint32_t pt_pfn= pde >> PFN_SHIFT;
     pte_t *page_table = (pte_t *)((char *)physical_memory + pt_pfn * PGSIZE);
 
     pte_t pte = page_table[ptx];
     if (!(pte & PTE_VALID)) {
         pthread_mutex_unlock(&vm_mutex);
         return NULL;
     }
 
     uint32_t data_pfn= pte >> PFN_SHIFT;
     void *page_base= (char *)physical_memory + data_pfn * PGSIZE;
 
    pthread_mutex_unlock(&vm_mutex);

    // Add this to TLB for next time
    TLB_add(va, page_base);

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
 
     pthread_mutex_lock(&tlb_mutex);
     if (tlb_lookups > 0) {
         miss_rate = (double)tlb_misses / (double)tlb_lookups;
     }
     pthread_mutex_unlock(&tlb_mutex);
 
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
     if (!pgdir || !va) {
         fprintf(stderr, "translate: pgdir or va is NULL\n");
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
 int map_page(pde_t *pgdir, void *va, void *pa)
 {
     if (!pgdir || !va || !pa) {
         return -1;
     }
 
    vaddr32_t vaddr = VA2U(va);
    // Get physical offset from the base of our memory
    paddr32_t paddr = (paddr32_t)((uintptr_t)pa - (uintptr_t)physical_memory);
 
     uint32_t pd_index = PDX(va);
     uint32_t pt_index = PTX(va);
 
    pde_t *pde_ptr    = &pgdir[pd_index];
    pte_t *page_table = NULL;

    // If we don't have a page table yet, create one
    if (*pde_ptr == 0) {
         page_table = (pte_t *)alloc_physical_page();
         if (!page_table) {
             perror("map_page: failed to allocate page table");
             return -1;
         }
 
         memset(page_table, 0, PGSIZE);
 
         paddr32_t pt_offset = (paddr32_t)((uintptr_t)page_table - (uintptr_t)physical_memory);
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
    // Look for num_pages consecutive free pages
    for (uint32_t i = 0; i <= total_virtual_pages - num_pages; i++) {
        bool found = true;
        
        // Check if all the pages we need starting at i are free
        for (int j = 0; j < num_pages; j++) {
            if (bitmap_get(virtual_bitmap, i + j)) {
                found = false;
                break;
            }
        }
        
        if (found) {
            // Mark them as used
            for (int j = 0; j < num_pages; j++) {
                bitmap_set(virtual_bitmap, i + j);
            }
            
            // Turn page number into a virtual address
            vaddr32_t va = i * PGSIZE;
            return U2VA(va);
        }
    }
    
    return NULL;  // Couldn't find enough space
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
     // Make sure VM is set up on first call
     pthread_once(&vm_init_once, set_physical_mem);

     if (!is_initialized || num_bytes == 0) {
         return NULL;
     }

     // Figure out how many pages we need (round up)
     int num_pages = (num_bytes + PGSIZE - 1) / PGSIZE;

     pthread_mutex_lock(&vm_mutex);

     // Find contiguous virtual pages (get_next_avail marks them as used)
     void *va_base = get_next_avail(num_pages);
     if (!va_base) {
         fprintf(stderr, "Error: Cannot find %d contiguous virtual pages\n", num_pages);
         pthread_mutex_unlock(&vm_mutex);
         return NULL;
     }
 
     vaddr32_t base_u      = VA2U(va_base);
     uint32_t  start_vpage = base_u / PGSIZE;
     int       mapped_pages = 0;  // track how many we've mapped so far

     for (int i = 0; i < num_pages; i++) {
         vaddr32_t page_va_u = base_u + (i * PGSIZE);
         void     *page_va   = U2VA(page_va_u);

         // Get a physical page
         void *pa = alloc_physical_page();
         if (!pa) {
             fprintf(stderr, "Error: Failed to allocate physical page %d/%d\n",
                     i + 1, num_pages);

             // Undo all the mappings we made so far
             for (int j = 0; j < mapped_pages; j++) {
                 vaddr32_t cleanup_va_u = base_u + (j * PGSIZE);
                 void     *cleanup_va   = U2VA(cleanup_va_u);
 
                 pte_t *pte = translate(page_directory, cleanup_va);
                 if (pte && (*pte & PTE_VALID)) {
                     uint32_t pfn = *pte >> PFN_SHIFT;
                     bitmap_clear(physical_bitmap, pfn);
                     *pte = 0;
                 }
             }
 
             // Clear all virtual bits reserved by get_next_avail 
             for (int j = 0; j < num_pages; j++) {
                 bitmap_clear(virtual_bitmap, start_vpage + j);
             }
 
             pthread_mutex_unlock(&vm_mutex);
             return NULL;
         }

         // Clear the physical page
         memset(pa, 0, PGSIZE);

         // Map this virtual page to the physical page
         if (map_page(page_directory, page_va, pa) < 0) {
             fprintf(stderr, "Error: Failed to map page %d/%d\n",
                     i + 1, num_pages);

             // Free the physical page we just got
             paddr32_t pt_offset = (paddr32_t)((uintptr_t)pa - (uintptr_t)physical_memory);
             uint32_t  pfn_new   = pt_offset >> OFFSET_BITS;
             bitmap_clear(physical_bitmap, pfn_new);

             // Undo previous mappings
             for (int j = 0; j < mapped_pages; j++) {
                 vaddr32_t cleanup_va_u = base_u + (j * PGSIZE);
                 void     *cleanup_va   = U2VA(cleanup_va_u);
 
                 pte_t *pte = translate(page_directory, cleanup_va);
                 if (pte && (*pte & PTE_VALID)) {
                     uint32_t pfn = *pte >> PFN_SHIFT;
                     bitmap_clear(physical_bitmap, pfn);
                     *pte = 0;
                 }
             }
 
             // Clear all virtual bits reserved by get_next_avail 
             for (int j = 0; j < num_pages; j++) {
                 bitmap_clear(virtual_bitmap, start_vpage + j);
             }
 
             pthread_mutex_unlock(&vm_mutex);
             return NULL;
         }
 
        mapped_pages++;
     }
 
    pthread_mutex_unlock(&vm_mutex);
    return va_base;
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
 
     int        num_pages   = (size + PGSIZE - 1) / PGSIZE;
     vaddr32_t  vaddr_base  = VA2U(va);
 
     pthread_mutex_lock(&vm_mutex);
 
     for (int i = 0; i < num_pages; i++) {
         vaddr32_t current_va  = vaddr_base + (i * PGSIZE);
         uint32_t  vpage_num   = current_va / PGSIZE;

         // Make sure this page was actually allocated before freeing it
         if (!bitmap_get(virtual_bitmap, vpage_num)) {
             fprintf(stderr,
                     "Warning: Trying to free unallocated virtual page at %p\n",
                     U2VA(current_va));
             continue;
         }

         // Get the page table entry
         pte_t *pte = translate(page_directory, U2VA(current_va));
         if (pte) {
             uint32_t pfn = *pte >> PFN_SHIFT;

             // Free the physical page and clear the entry
             bitmap_clear(physical_bitmap, pfn);
             *pte = 0;
 
             pthread_mutex_lock(&tlb_mutex);
             uint32_t vpn = current_va >> OFFSET_BITS;
             for (int t = 0; t < TLB_ENTRIES; t++) {
                 if (tlb_store[t].valid && tlb_store[t].vpn == vpn) {
                     tlb_store[t].valid = false;
                 }
            }
            pthread_mutex_unlock(&tlb_mutex);
         } else {
             fprintf(stderr,
                     "Warning: Page table entry missing while freeing VA %p\n",
                     U2VA(current_va));
         }

         // Mark the virtual page as free
         bitmap_clear(virtual_bitmap, vpage_num);
     }
 
    pthread_mutex_unlock(&vm_mutex);

    // printf("n_free: Freed %d bytes (%d pages) starting at VA %p\n",
    //         size, num_pages, va);
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
     int remaining         = size;
     int src_offset        = 0;
 
     while (remaining > 0) {
         void *pa = get_physical_addr(U2VA(current_va));
         if (!pa) {
             fprintf(stderr, "Error: Virtual address %p not mapped in put_data\n",
                     U2VA(current_va));
             return -1;
         }
 
         uint32_t page_offset   = OFF(U2VA(current_va));
         uint32_t bytes_in_page = PGSIZE - page_offset;
         if (bytes_in_page > remaining) {
             bytes_in_page = remaining;
         }
 
         memcpy(pa, (char *)val + src_offset, bytes_in_page);
 
         remaining  -= bytes_in_page;
         src_offset += bytes_in_page;
         current_va += bytes_in_page;
     }
 
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
     int remaining         = size;
     int dst_offset        = 0;
 
     while (remaining > 0) {
         void *pa = get_physical_addr(U2VA(current_va));
         if (!pa) {
             fprintf(stderr, "Error: Virtual address %p not mapped in get_data\n",
                     U2VA(current_va));
             return;
         }
 
         uint32_t page_offset   = OFF(U2VA(current_va));
         uint32_t bytes_in_page = PGSIZE - page_offset;
         if (bytes_in_page > remaining) {
             bytes_in_page = remaining;
         }
 
         memcpy((char *)val + dst_offset, pa, bytes_in_page);
 
         remaining  -= bytes_in_page;
         dst_offset += bytes_in_page;
         current_va += bytes_in_page;
     }
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
                // Figure out where mat1[i][k] and mat2[k][j] are
                // Row-major order: A[i][j] = A + (i * size + j) * sizeof(element)
                vaddr32_t addr_mat1 = VA2U(mat1) + ((i * size + k) * sizeof(uint32_t));
                vaddr32_t addr_mat2 = VA2U(mat2) + ((k * size + j) * sizeof(uint32_t));
                
                uint32_t a, b;
                
                // Grab mat1[i][k]
                get_data(U2VA(addr_mat1), &a, sizeof(uint32_t));
                
                // Grab mat2[k][j]
                get_data(U2VA(addr_mat2), &b, sizeof(uint32_t));
                
                // Add to running sum
                sum += (a * b);
            }
            
            // Figure out where answer[i][j] goes
            vaddr32_t addr_answer = VA2U(answer) + ((i * size + j) * sizeof(uint32_t));
            
            // Store the result
            put_data(U2VA(addr_answer), &sum, sizeof(uint32_t));
        }
        
        // Show progress for big matrices
        if ((i + 1) % 10 == 0) {
            printf("  Completed row %d/%d\n", i + 1, size);
        }
    }
    
    printf("Matrix multiplication complete\n");
}

