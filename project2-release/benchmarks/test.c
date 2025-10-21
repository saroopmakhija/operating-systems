#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../thread-worker.h"

void* simple_function(void* arg) {
    int id = *(int*)arg;
    printf("[THREAD %d] Started\n", id);
    fflush(stdout);
    
    for(int i = 0; i < 5; i++) {
        printf("[THREAD %d] Loop iteration %d\n", id, i);
        fflush(stdout);
        
        // Do some work to consume CPU time
        volatile int x = 0;
        for(int j = 0; j < 1000000; j++) {
            x += j;
        }
    }
    
    printf("[THREAD %d] Finished\n", id);
    fflush(stdout);
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t threads[3];
    int ids[3] = {1, 2, 3};
    
    printf("[MAIN] Starting program\n");
    fflush(stdout);
    
    printf("[MAIN] Creating 3 threads\n");
    fflush(stdout);
    
    for (int i = 0; i < 3; i++) {
        printf("[MAIN] Creating thread %d\n", i+1);
        fflush(stdout);
        int ret = pthread_create(&threads[i], NULL, simple_function, &ids[i]);
        printf("[MAIN] Thread %d created, returned %d\n", i+1, ret);
        fflush(stdout);
    }
    
    printf("[MAIN] All threads created, now joining them\n");
    fflush(stdout);
    
    for (int i = 0; i < 3; i++) {
        printf("[MAIN] Joining thread %d\n", i+1);
        fflush(stdout);
        pthread_join(threads[i], NULL);
        printf("[MAIN] Thread %d joined\n", i+1);
        fflush(stdout);
    }
    
    printf("[MAIN] All threads completed, exiting\n");
    fflush(stdout);
    
    return 0;
}