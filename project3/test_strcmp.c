#include <stdio.h>
#include <string.h>
int main() {
    char write_data[20] = "CROSS_PAGE_TEST_DATA";
    char read_data[20];
    memcpy(read_data, write_data, 20);
    
    printf("memcmp result: %d\n", memcmp(read_data, write_data, 20));
    printf("strcmp result: %d\n", strcmp(read_data, write_data));
    
    return 0;
}
