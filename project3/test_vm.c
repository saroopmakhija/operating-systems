// -----------------------------------------------------------------------------
// test_vm.c - Comprehensive test for Part 1
// -----------------------------------------------------------------------------

#include "my_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

// Test 1: Basic allocation and deallocation
void test_basic_alloc_free() {
    printf("\n=== Test 1: Basic Allocation and Deallocation ===\n");
    
    // Allocate 1 byte (should allocate 1 page)
    void *ptr1 = n_malloc(1);
    assert(ptr1 != NULL);
    printf("✓ Allocated 1 byte at %p\n", ptr1);
    
    // Allocate 4096 bytes (should allocate 1 page)
    void *ptr2 = n_malloc(4096);
    assert(ptr2 != NULL);
    printf("✓ Allocated 4096 bytes at %p\n", ptr2);
    
    // Allocate 8000 bytes (should allocate 2 pages)
    void *ptr3 = n_malloc(8000);
    assert(ptr3 != NULL);
    printf("✓ Allocated 8000 bytes at %p\n", ptr3);
    
    // Free memory
    n_free(ptr1, 1);
    printf("✓ Freed ptr1\n");
    
    n_free(ptr2, 4096);
    printf("✓ Freed ptr2\n");
    
    n_free(ptr3, 8000);
    printf("✓ Freed ptr3\n");
    
    printf("Test 1 PASSED\n");
}

// Test 2: Put and Get data
void test_put_get_data() {
    printf("\n=== Test 2: Put and Get Data ===\n");
    
    void *ptr = n_malloc(100);
    assert(ptr != NULL);
    
    // Test integer
    int write_val = 42;
    int read_val = 0;
    
    int result = put_data(ptr, &write_val, sizeof(int));
    assert(result == 0);
    printf("✓ Wrote value %d\n", write_val);
    
    get_data(ptr, &read_val, sizeof(int));
    assert(read_val == write_val);
    printf("✓ Read value %d (expected %d)\n", read_val, write_val);
    
    // Test array
    int array[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    int read_array[10] = {0};
    
    result = put_data(ptr, array, sizeof(array));
    assert(result == 0);
    printf("✓ Wrote array of 10 integers\n");
    
    get_data(ptr, read_array, sizeof(read_array));
    for (int i = 0; i < 10; i++) {
        assert(read_array[i] == array[i]);
    }
    printf("✓ Read array matches written array\n");
    
    n_free(ptr, 100);
    printf("Test 2 PASSED\n");
}

// Test 3: Cross-page data
void test_cross_page_data() {
    printf("\n=== Test 3: Cross-Page Data Access ===\n");
    
    // Allocate 2 pages
    void *ptr = n_malloc(8192);
    assert(ptr != NULL);
    
    // Write data that spans across page boundary
    // Place data at offset 4090 (6 bytes before end of first page)
    char *offset_ptr = (char *)ptr + 4090;
    
    char write_data[21] = "CROSS_PAGE_TEST_DATA";
    char read_data[21] = {0};
    
    int result = put_data(offset_ptr, write_data, sizeof(write_data));
    assert(result == 0);
    printf("✓ Wrote data spanning page boundary\n");
    
    get_data(offset_ptr, read_data, sizeof(read_data));
    assert(strcmp(read_data, write_data) == 0);
    printf("✓ Read data: '%s' (expected '%s')\n", read_data, write_data);
    
    n_free(ptr, 8192);
    printf("Test 3 PASSED\n");
}

// Test 4: Multiple allocations and fragmentation
void test_multiple_allocations() {
    printf("\n=== Test 4: Multiple Allocations ===\n");
    
    void *ptrs[10];
    
    // Allocate 10 blocks
    for (int i = 0; i < 10; i++) {
        ptrs[i] = n_malloc(1000);
        assert(ptrs[i] != NULL);
        
        // Write unique value to each block
        int value = i * 100;
        put_data(ptrs[i], &value, sizeof(int));
    }
    printf("✓ Allocated 10 blocks\n");
    
    // Verify all values
    for (int i = 0; i < 10; i++) {
        int value;
        get_data(ptrs[i], &value, sizeof(int));
        assert(value == i * 100);
    }
    printf("✓ All values verified\n");
    
    // Free every other block
    for (int i = 0; i < 10; i += 2) {
        n_free(ptrs[i], 1000);
    }
    printf("✓ Freed every other block\n");
    
    // Verify remaining blocks
    for (int i = 1; i < 10; i += 2) {
        int value;
        get_data(ptrs[i], &value, sizeof(int));
        assert(value == i * 100);
    }
    printf("✓ Remaining blocks still valid\n");
    
    // Free remaining blocks
    for (int i = 1; i < 10; i += 2) {
        n_free(ptrs[i], 1000);
    }
    
    printf("Test 4 PASSED\n");
}

// Test 5: Small matrix multiplication
void test_matrix_mult() {
    printf("\n=== Test 5: Matrix Multiplication ===\n");
    
    int size = 3;
    int mat_size = size * size * sizeof(uint32_t);
    
    // Allocate matrices
    void *mat1 = n_malloc(mat_size);
    void *mat2 = n_malloc(mat_size);
    void *answer = n_malloc(mat_size);
    
    assert(mat1 != NULL && mat2 != NULL && answer != NULL);
    
    // Initialize mat1 (identity matrix)
    uint32_t identity[3][3] = {
        {1, 0, 0},
        {0, 1, 0},
        {0, 0, 1}
    };
    put_data(mat1, identity, mat_size);
    
    // Initialize mat2
    uint32_t mat[3][3] = {
        {1, 2, 3},
        {4, 5, 6},
        {7, 8, 9}
    };
    put_data(mat2, mat, mat_size);
    
    // Perform multiplication
    mat_mult(mat1, mat2, size, answer);
    
    // Read and verify result
    uint32_t result[3][3];
    get_data(answer, result, mat_size);
    
    printf("Result matrix:\n");
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            printf("%3u ", result[i][j]);
        }
        printf("\n");
    }
    
    // Verify (identity * mat = mat)
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            assert(result[i][j] == mat[i][j]);
        }
    }
    printf("✓ Matrix multiplication correct\n");
    
    n_free(mat1, mat_size);
    n_free(mat2, mat_size);
    n_free(answer, mat_size);
    
    printf("Test 5 PASSED\n");
}

// Test 6: Thread safety (basic)
void *thread_alloc_free(void *arg) {
    int thread_id = *(int *)arg;
    
    for (int i = 0; i < 5; i++) {
        void *ptr = n_malloc(1024);
        if (ptr) {
            int value = thread_id * 1000 + i;
            put_data(ptr, &value, sizeof(int));
            
            int read_val;
            get_data(ptr, &read_val, sizeof(int));
            
            if (read_val != value) {
                printf("Thread %d: Value mismatch! Expected %d, got %d\n", 
                       thread_id, value, read_val);
            }
            
            n_free(ptr, 1024);
        }
    }
    
    return NULL;
}

void test_thread_safety() {
    printf("\n=== Test 6: Thread Safety ===\n");
    
    pthread_t threads[4];
    int thread_ids[4] = {1, 2, 3, 4};
    
    // Create threads
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, thread_alloc_free, &thread_ids[i]);
    }
    
    // Wait for threads
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("✓ All threads completed successfully\n");
    printf("Test 6 PASSED\n");
}

// Main test runner
int main() {
    printf("========================================\n");
    printf("  Virtual Memory System Test Suite\n");
    printf("========================================\n");
    
    test_basic_alloc_free();
    test_put_get_data();
    test_cross_page_data();
    test_multiple_allocations();
    test_matrix_mult();
    test_thread_safety();
    
    printf("\n========================================\n");
    printf("  ALL TESTS PASSED!\n");
    printf("========================================\n");
    
    return 0;
}