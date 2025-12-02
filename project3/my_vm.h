#ifndef MY_VM_H_INCLUDED
#define MY_VM_H_INCLUDED

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 *  Virtual Memory Simulation Header
 * ============================================================================
 *  32-bit virtual address space, 2-level page table, software TLB.
 * ============================================================================
 */

// -----------------------------------------------------------------------------
//  Memory and Paging Configuration
// -----------------------------------------------------------------------------

#define VA_BITS        32u           // Simulated virtual address width

// Change this when testing different page sizes (4KB to 64KB)
#define PGSIZE         32768u         // Page size in bytes

#define MAX_MEMSIZE    (1ULL << 32)  // Max virtual memory = 4 GB
#define MEMSIZE        (1ULL << 30)  // Simulated physical memory = 1 GB


#if   PGSIZE == 4096u       /* 4KB  */
    #define OFFSET_BITS  12u
    #define PTX_BITS     10u
    #define PDX_BITS     10u
#elif PGSIZE == 8192u       /* 8KB  */
    #define OFFSET_BITS  13u
    #define PTX_BITS      9u
    #define PDX_BITS     10u
#elif PGSIZE == 16384u      /* 16KB */
    #define OFFSET_BITS  14u
    #define PTX_BITS      9u
    #define PDX_BITS      9u
#elif PGSIZE == 32768u      /* 32KB */
    #define OFFSET_BITS  15u
    #define PTX_BITS      8u
    #define PDX_BITS      9u
#elif PGSIZE == 65536u      /* 64KB */
    #define OFFSET_BITS  16u
    #define PTX_BITS      8u
    #define PDX_BITS      8u
#else
    #error "Unsupported PGSIZE. Supported: 4KB, 8KB, 16KB, 32KB, 64KB."
#endif

// Valid bit for page table entries (LSB = 1 means the mapping is valid)
#define PTE_VALID      1u

// --- Constants for bit shifts and masks ---
#define PTXSHIFT       (OFFSET_BITS)
#define PDXSHIFT       (OFFSET_BITS + PTX_BITS)

#define OFFMASK        ((1u << OFFSET_BITS) - 1u)
#define PTXMASK        ((1u << PTX_BITS)   - 1u)
#define PDXMASK        ((1u << PDX_BITS)   - 1u)

// Legacy name - keeping this for compatibility with older code
#define PXMASK         PTXMASK

// Shift amount for physical frame numbers in page table entries
#define PFN_SHIFT      (OFFSET_BITS)
// Old name kept for backwards compatibility
#define PFNSHIFT       PFN_SHIFT

// -----------------------------------------------------------------------------
//  Type Definitions
// -----------------------------------------------------------------------------

typedef uint32_t vaddr32_t;   // Simulated 32-bit virtual address
typedef uint32_t paddr32_t;   // Simulated 32-bit physical address
typedef uint32_t pte_t;       // Page table entry
typedef uint32_t pde_t;       // Page directory entry

// -----------------------------------------------------------------------------
//  Address Conversion Helpers (Provided)
// -----------------------------------------------------------------------------

static inline vaddr32_t VA2U(void *va)    { return (vaddr32_t)(uintptr_t)va; }
static inline void*     U2VA(vaddr32_t u) { return (void*)(uintptr_t)u; }

// --- Macros to extract address components ---
#define PDX(va)        ((VA2U(va) >> PDXSHIFT) & PDXMASK)
#define PTX(va)        ((VA2U(va) >> PTXSHIFT) & PTXMASK)
#define OFF(va)        ( VA2U(va) & OFFMASK )

// -----------------------------------------------------------------------------
//  TLB Configuration
// -----------------------------------------------------------------------------

#define TLB_ENTRIES    512   // Number of TLB entries (can change this if needed)

struct tlb {
    // Each TLB entry stores:
    //   vpn   : virtual page number
    //   pfn   : physical frame number
    //   valid : whether this entry is currently valid
    uint32_t vpn;
    uint32_t pfn;
    bool     valid;
    // Could add uint64_t last_used here for LRU if we wanted
};

// -----------------------------------------------------------------------------
//  Helper Functions
// -----------------------------------------------------------------------------

void bitmap_set   (unsigned char *bitmap, int page_index);
void bitmap_clear (unsigned char *bitmap, int page_index);
int  bitmap_get   (unsigned char *bitmap, int page_index);

// -----------------------------------------------------------------------------
//  Main API Functions
// -----------------------------------------------------------------------------

/*
 * Initializes physical memory and supporting data structures.
 * Return: None.
 */
void set_physical_mem(void);

/*
 * Adds a new virtual-to-physical-page translation to the TLB.
 *   va: virtual address (any address within the page)
 *   pa: physical address of the start of that page (page base)
 * Return: 0 on success, -1 on failure.
 */
int TLB_add(void *va, void *pa);

/*
 * Checks if a virtual address translation exists in the TLB.
 * Return:
 *   - On hit : physical address (void*) corresponding to this VA
 *   - On miss: NULL
 */
void *TLB_check(void *va);

/*
 * Calculates and prints the TLB miss rate.
 * Return: None.
 */
void print_TLB_missrate(void);

/*
 * Translates a virtual address to a PTE* by walking the 2-level page table.
 * (TLB not used here directly; your higher-level code can use TLB first.)
 * Return: pointer to PTE if successful; NULL otherwise.
 */
pte_t *translate(pde_t *pgdir, void *va);

/*
 * Creates a mapping between a virtual page and a physical page.
 *   pgdir : pointer to top-level page directory
 *   va    : virtual address (page-aligned base of VA page)
 *   pa    : physical address (page-aligned base of PA frame)
 * Return: 0 on success, -1 on failure.
 */
int map_page(pde_t *pgdir, void *va, void *pa);

/*
 * Finds the next available block of contiguous virtual pages.
 * Return: pointer to base virtual address on success; NULL if unavailable.
 */
void *get_next_avail(int num_pages);

/*
 * Allocates memory in the simulated virtual address space.
 * Return: pointer to base virtual address on success; NULL on failure.
 */
void *n_malloc(unsigned int num_bytes);

/*
 * Frees one or more pages of memory starting from the given virtual address.
 * Return: None.
 */
void n_free(void *va, int size);

/*
 * Copies data from a user buffer into simulated physical memory
 * through a virtual address (handles page crossing).
 * Return: 0 on success, -1 on failure.
 */
int put_data(void *va, void *val, int size);

/*
 * Copies data from simulated physical memory into a user buffer.
 * Return: None.
 */
void get_data(void *va, void *val, int size);

/*
 * Performs matrix multiplication using data stored in simulated memory.
 * Each element should be accessed via get_data() and stored via put_data().
 * Return: None.
 */
void mat_mult(void *mat1, void *mat2, int size, void *answer);

#endif // MY_VM_H_INCLUDED
