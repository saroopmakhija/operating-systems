#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>   // for SIGSTKSZ

#define STACK_SIZE SIGSTKSZ

// Global contexts and stack
static ucontext_t main_ctx, worker1_ctx;
static void *w1_stack = NULL;

// guard to detect resume into main via uc_link (after worker returns)
static volatile int resumed_to_main = 0;

//A simple worker function that simply prints and returns
static void worker1(void) {
    printf("In worker: started\n");
    printf("In worker: returning (uc_link will switch back to main)\n");
}



//Function to initialize the context
static void init_context(ucontext_t *ctx, void **stack_ptr,
                         void (*fn)(void), ucontext_t *link_ctx) {
    if (getcontext(ctx) == -1) {
        perror("getcontext");
        exit(1);
    }

    *stack_ptr = malloc(STACK_SIZE);
    if (!*stack_ptr) {
        perror("malloc(stack)");
        exit(1);
    }

    ctx->uc_stack.ss_sp = *stack_ptr;
    ctx->uc_stack.ss_size = STACK_SIZE;
    ctx->uc_stack.ss_flags = 0;
    ctx->uc_link = link_ctx;

    makecontext(ctx, fn, 0);
}


int main(void) {
    printf("In main: saving main context with getcontext\n");

    if (getcontext(&main_ctx) == -1) {
        perror("getcontext(main)");
        return 1;
    }

    //ADD YOUR CODE HERE








    // Initialize worker context; link back to main
    init_context(&worker1_ctx, &w1_stack, worker1, &main_ctx);

    printf("In main: transferring control to worker using setcontext\n");
    if (setcontext(&worker1_ctx) == -1) {
        perror("setcontext(worker1)");
        return 1;
    }

    // Unreachable: setcontext does not return
    printf("In main: unreachable after setcontext\n");
    return 0;
}
