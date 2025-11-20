
#include "my_vm.h"
#include <string.h>   // optional for memcpy if you later implement put/get

// -----------------------------------------------------------------------------
// Global Declarations (optional)
// -----------------------------------------------------------------------------

static void *physical_memory = NULL;      // Simulated physical RAM
static pde_t *page_directory = NULL;      // Top-level page directory
static unsigned char *physical_bitmap = NULL;  // Track physical pages
static unsigned char *virtual_bitmap = NULL;   // Track virtual pages
static bool is_initialized = false;
static pthread_mutex_t vm_mutex = PTHREAD_MUTEX_INITIALIZER;

struct tlb tlb_store; // Placeholder for your TLB structure

// Optional counters for TLB statistics
static unsigned long long tlb_lookups = 0;
static unsigned long long tlb_misses  = 0;

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
/*
 * set_physical_mem()
 * ------------------
 * Allocates and initializes simulated physical memory and any required
 * data structures (e.g., bitmaps for tracking page use).
 *
 * Return value: None.
 * Errors should be handled internally (e.g., failed allocation).
 */

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

void bitmap_get(unsigned char *bitmap, int page_index) {
    return bitmap[page_index / 8] & (1 << (page_index % 8));
}





void set_physical_mem(void) {
    // TODO: Implement memory allocation for simulated physical memory.
    // Use 32-bit values for sizes, page counts, and offsets.
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

    is_initialized = true;
    pthread_mutex_unlock(&vm_mutex);

    printf("Virtual memory initialized:\n");
    printf("  Physical memory: %u pages (%u MB)\n", 
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

    if (!pgdir || !va) {
        fprintf(stderr, "translate: pgdir or va is NULL\n");
        return NULL;
    }



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
    // TODO: Map virtual address to physical address in the page tables.
    return -1; // Failure placeholder.
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------

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
    // TODO: Implement virtual bitmap search for free pages.
    return NULL; // No available block placeholder.
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
    // TODO: Determine required pages, allocate them, and map them.
    return NULL; // Allocation failure placeholder.
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
    // TODO: Clear page table entries, update bitmaps, and invalidate TLB.
    


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
    // TODO: Walk virtual pages, translate to physical addresses,
    // and copy data into simulated memory.
    



    return -1; // Failure placeholder.
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
    // TODO: Perform reverse operation of put_data().
    //
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
    int i, j, k;
    uint32_t a, b, c;

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            c = 0;
            for (k = 0; k < size; k++) {
                // TODO: Compute addresses for mat1[i][k] and mat2[k][j].
                // Retrieve values using get_data() and perform multiplication.
                get_data(NULL, &a, sizeof(int));  // placeholder
                get_data(NULL, &b, sizeof(int));  // placeholder
                c += (a * b);
            }
            // TODO: Store the result in answer[i][j] using put_data().
            put_data(NULL, (void *)&c, sizeof(int)); // placeholder
        }
    }
}

