// ssm229 Saroop Makhija
// mk2177 Aiman Koli 
#ifndef MY_VM64_H_INCLUDED
#define MY_VM64_H_INCLUDED

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VA64_BITS       48u
#define PGSIZE64        4096u

// VA = [ L4 | L3 | L2 | L1 | offset ]
#define OFF64_BITS      12u
#define LVL_BITS        9u

#define L1_SHIFT        (OFF64_BITS)
#define L2_SHIFT        (L1_SHIFT + LVL_BITS)
#define L3_SHIFT        (L2_SHIFT + LVL_BITS)
#define L4_SHIFT        (L3_SHIFT + LVL_BITS)

#define LVL_MASK        ((1ULL << LVL_BITS) - 1ULL)
#define OFF64_MASK      ((1ULL << OFF64_BITS) - 1ULL)

#define MEMSIZE64       (1ULL << 30)

typedef uint64_t vaddr64_t;
typedef uint64_t paddr64_t;
typedef uint64_t pte64_t;
typedef uint64_t pde64_t;

#define PTE64_VALID     (1ULL << 0)
#define PFN64_SHIFT     (OFF64_BITS)

#define TLB64_ENTRIES   512

struct tlb64_entry {
    uint64_t vpn;
    uint64_t pfn;
    bool     valid;
};

void vm64_init(void);
vaddr64_t vm64_malloc(size_t bytes);
void vm64_free(vaddr64_t va, size_t bytes);
int vm64_put(vaddr64_t va, const void *src, size_t size);
int vm64_get(vaddr64_t va, void *dst, size_t size);
void vm64_mat_mult(vaddr64_t mat1, vaddr64_t mat2, int n, vaddr64_t out);
void vm64_print_TLB_missrate(void);

#endif // MY_VM64_H_INCLUDED
