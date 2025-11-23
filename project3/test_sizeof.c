#include <stdio.h>
int main() {
    char write_data[20] = "CROSS_PAGE_TEST_DATA";
    printf("sizeof(write_data) = %zu\n", sizeof(write_data));
    printf("String: '");
    for (int i = 0; i < sizeof(write_data); i++) {
        if (write_data[i] == 0) printf("\\0");
        else printf("%c", write_data[i]);
    }
    printf("'\n");
    return 0;
}
