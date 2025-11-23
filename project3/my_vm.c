
#include "my_vm.h"
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

// Memory configuration
static uint32_t total_physical_pages;
static uint32_t total_virtual_pages;
static uint32_t bitmap_size;

struct tlb tlb_store; // Placeholder for your TLB structure

// Optional counters for TLB statistics
static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses  = 0;

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
    if (!is_initialized) {
        pthread_mutexattr_init(&vm_mutex_attr);
        pthread_mutexattr_settype(&vm_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&vm_mutex, &vm_mutex_attr);
    }
    
    pthread_mutex_lock(&vm_mutex);
    if (is_initialized) {
        fprintf(stderr, "physical memory already initialized\n");
        pthread_mutex_unlock(&vm_mutex);
        return;
    }
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
    virtual_bitmap = calloc(virtual_bitmap_size, 1);
    if (!physical_bitmap || !virtual_bitmap) {
        fprintf(stderr, "failed to allocate memory for physical memory\n");
        return;
    }
    
    page_directory = (pde_t *)physical_memory; // put page directory at the start of physical mem
    memset(physical_memory, 0, PGSIZE); // the page directory is 4kb

    //because of this allocation the first page of physical mem is used. mark as used
    bitmap_set(physical_bitmap, 0);
    
    // Reserve virtual page 0 (so we never return NULL as a valid address)
    bitmap_set(virtual_bitmap, 0);

    is_initialized = true;
    pthread_mutex_unlock(&vm_mutex);

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
    // TODO: Implement TLB insertion logic.
    return -1; // Currently returns failure placeholder.
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
pte_t *TLB_check(void *va)
{
    // TODO: Implement TLB lookup.
    return NULL; // Currently returns TLB miss.
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
    // TODO: Calculate miss rate as (tlb_misses / tlb_lookups).
    fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
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
    int off = OFF(va);

    pthread_mutex_lock(&vm_mutex);
    pde_t pde = pgdir[pdx]; // get the page directory entry
    if (!pde || !(pde & PTE_VALID) || pde==0) {
        fprintf(stderr, "translate: page directory entry not found\n");
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }
    uintptr_t pfn = pde >> PFNSHIFT;
    pte_t *page_table = (pte_t *)physical_memory + (pfn * PGSIZE);
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
        paddr32_t pt_offset = (paddr32_t)(uintptr_t)page_table - (paddr32_t)(uintptr_t)physical_memory;
        *pde_ptr = (pt_offset >> OFFSET_BITS) | PTE_VALID;
    } else {
        // Page table exists, retrieve it
        uint32_t pt_pfn = *pde_ptr >> OFFSET_BITS;
        page_table = (pte_t *)((char *)physical_memory + (pt_pfn * PGSIZE));
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
    if (!is_initialized) {
        set_physical_mem();
    }
    
    if (num_bytes == 0) {
        return NULL;
    }
    
    // Calculate number of pages needed (round up)
    int num_pages = (num_bytes + PGSIZE - 1) / PGSIZE;
    
    pthread_mutex_lock(&vm_mutex);
    
    // Find contiguous virtual pages
    void *va_base = get_next_avail(num_pages);
    if (!va_base) {
        fprintf(stderr, "Error: Cannot find %d contiguous virtual pages\n", num_pages);
        pthread_mutex_unlock(&vm_mutex);
        return NULL;
    }
    
    // Allocate physical pages and create mappings
    for (int i = 0; i < num_pages; i++) {
        // Calculate virtual address for this page
        vaddr32_t va = VA2U(va_base) + (i * PGSIZE);
        
        // Allocate a physical page
        void *pa = alloc_physical_page();
        if (!pa) {
            fprintf(stderr, "Error: Failed to allocate physical page %d/%d\n", i+1, num_pages);
            
            // Clean up previously allocated pages
            for (int j = 0; j < i; j++) {
                vaddr32_t cleanup_va = VA2U(va_base) + (j * PGSIZE);
                uint32_t vpage = cleanup_va / PGSIZE;
                bitmap_clear(virtual_bitmap, vpage);
                // TODO: Free physical pages and clear page table entries
            }
            
            pthread_mutex_unlock(&vm_mutex);
            return NULL;
        }
        
        // Initialize the physical page to zero
        memset(pa, 0, PGSIZE);
        
        // Create mapping in page table
        if (map_page(page_directory, U2VA(va), pa) < 0) {
            fprintf(stderr, "Error: Failed to map page %d/%d\n", i+1, num_pages);
            pthread_mutex_unlock(&vm_mutex);
            return NULL;
        }
    }
    
    pthread_mutex_unlock(&vm_mutex);
    
    printf("n_malloc: Allocated %d bytes (%d pages) at VA %p\n", 
           num_bytes, num_pages, va_base);
    
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
    
    // Calculate number of pages to free
    int num_pages = (size + PGSIZE - 1) / PGSIZE;
    vaddr32_t vaddr_base = VA2U(va);
    
    pthread_mutex_lock(&vm_mutex);
    
    for (int i = 0; i < num_pages; i++) {
        vaddr32_t current_va = vaddr_base + (i * PGSIZE);
        uint32_t vpage_num = current_va / PGSIZE;
        
        // Check if virtual page is actually allocated
        if (!bitmap_get(virtual_bitmap, vpage_num)) {
            fprintf(stderr, "Warning: Trying to free unallocated virtual page at %p\n", 
                    U2VA(current_va));
            continue;
        }
        
        // Get page table entry
        pte_t *pte = translate(page_directory, U2VA(current_va));
        
        if (pte) {
            // Get physical frame number
            uint32_t pfn = *pte >> PFN_SHIFT;
            
            // Free the physical page
            bitmap_clear(physical_bitmap, pfn);
            
            // Clear the page table entry
            *pte = 0;
        }
        
        // Free the virtual page
        bitmap_clear(virtual_bitmap, vpage_num);
    }
    
    pthread_mutex_unlock(&vm_mutex);
    
    printf("n_free: Freed %d bytes (%d pages) at VA %p\n", 
           size, num_pages, va);
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
        // Translate virtual address to get PTE
        pte_t *pte = translate(page_directory, U2VA(current_va));
        
        if (!pte) {
            fprintf(stderr, "Error: Virtual address %p not mapped\n", U2VA(current_va));
            pthread_mutex_unlock(&vm_mutex);
            return -1;
        }
        
        // Get physical frame number
        uint32_t pfn = *pte >> PFN_SHIFT;
        
        // Calculate offset within the page
        uint32_t page_offset = OFF(U2VA(current_va));
        
        // Calculate how many bytes to copy in this page
        uint32_t bytes_in_page = PGSIZE - page_offset;
        if (bytes_in_page > remaining) {
            bytes_in_page = remaining;
        }
        
        // Calculate physical address
        void *physical_addr = (char *)physical_memory + (pfn * PGSIZE) + page_offset;
        
        // Copy data
        memcpy(physical_addr, (char *)val + src_offset, bytes_in_page);
        
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
        // Translate virtual address to get PTE
        pte_t *pte = translate(page_directory, U2VA(current_va));
        
        if (!pte) {
            fprintf(stderr, "Error: Virtual address %p not mapped\n", U2VA(current_va));
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        
        // Get physical frame number
        uint32_t pfn = *pte >> PFN_SHIFT;
        
        // Calculate offset within the page
        uint32_t page_offset = OFF(U2VA(current_va));
        
        // Calculate how many bytes to copy in this page
        uint32_t bytes_in_page = PGSIZE - page_offset;
        if (bytes_in_page > remaining) {
            bytes_in_page = remaining;
        }
        
        // Calculate physical address
        void *physical_addr = (char *)physical_memory + (pfn * PGSIZE) + page_offset;
        
        // Copy data
        memcpy((char *)val + dst_offset, physical_addr, bytes_in_page);
        
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

