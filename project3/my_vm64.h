#ifndef MY_VM64_H_INCLUDED
#define MY_VM64_H_INCLUDED

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * 64-bit virtual memory simulation with a 4-level page table and a simple
 * direct-mapped TLB. This is an extra-credit style implementation meant
 * for studying, separate from the 32-bit 2-level design.
 */

// ---------------------------------------------------------------------------
// Basic configuration
// ---------------------------------------------------------------------------

#define VA64_BITS       48u           // canonical 48-bit virtual addresses
#define PGSIZE64        4096u         // 4KB pages

#define OFF64_BITS      12u           // offset within page
#define LVL_BITS        9u            // bits per level (PML4, PDPT, PD, PT)

#define L1_SHIFT        (OFF64_BITS)
#define L2_SHIFT        (L1_SHIFT + LVL_BITS)
#define L3_SHIFT        (L2_SHIFT + LVL_BITS)
#define L4_SHIFT        (L3_SHIFT + LVL_BITS)

#define LVL_MASK        ((1ULL << LVL_BITS) - 1ULL)
#define OFF64_MASK      ((1ULL << OFF64_BITS) - 1ULL)

// Simulated physical memory size (1 GiB). Virtual space is conceptually
// 48-bit but we only allocate a small contiguous prefix of it.
#define MEMSIZE64       (1ULL << 30)   // 1 GiB of fake physical memory

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef uint64_t vaddr64_t;  // simulated 64-bit virtual address
typedef uint64_t paddr64_t;  // simulated 64-bit physical address
typedef uint64_t pte64_t;    // 64-bit page-table entry
typedef uint64_t pde64_t;    // 64-bit page-directory entry (same as pte)

// PTE flags and PFN encoding
#define PTE64_VALID     (1ULL << 0)
#define PFN64_SHIFT     (OFF64_BITS)   // PFN is stored above offset bits

// ---------------------------------------------------------------------------
// TLB configuration (direct-mapped)
// ---------------------------------------------------------------------------

#define TLB64_ENTRIES   512

struct tlb64_entry {
    uint64_t vpn;   // virtual page number
    uint64_t pfn;   // physical frame number
    bool     valid;
};

// ---------------------------------------------------------------------------
// Public API (64-bit VM)
// ---------------------------------------------------------------------------

// Initialize fake physical memory, 4-level page tables and TLB.
void vm64_init(void);

// Allocate 'bytes' of virtual memory, returning the base virtual address.
vaddr64_t vm64_malloc(size_t bytes);

// Free 'bytes' of virtual memory starting at virtual address 'va'.
void vm64_free(vaddr64_t va, size_t bytes);

// Copy 'size' bytes from user buffer 'src' into simulated memory at 'va'.
int vm64_put(vaddr64_t va, const void *src, size_t size);

// Copy 'size' bytes from simulated memory at 'va' into user buffer 'dst'.
int vm64_get(vaddr64_t va, void *dst, size_t size);

// Matrix multiply two square matrices stored in simulated memory.
// Each matrix is 'n x n' of int values.
void vm64_mat_mult(vaddr64_t mat1, vaddr64_t mat2, int n, vaddr64_t out);

// Print current TLB miss rate statistics.
void vm64_print_TLB_missrate(void);

#endif // MY_VM64_H_INCLUDED
