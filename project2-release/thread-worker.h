// List all group member's name:
// username of iLab:
// iLab Server:
#ifndef WORKER_T_H
#define WORKER_T_H
#define _GNU_SOURCE

/* To use Linux pthread Library in Benchmark, you have to comment the USE_WORKERS macro */
#define USE_WORKERS 1

/* Targeted latency in milliseconds */
#define TARGET_LATENCY 20

/* Minimum scheduling granularity in milliseconds */
#define MIN_SCHED_GRN 1

/* Time slice quantum in milliseconds */
#define QUANTUM 10

/*Number of quantums after which MLFQ resets*/
#define S 40

#define NUM_PRIORITIES 10

/*Number of quantums to run for per priority level*/
#define QUANTUMS_PER_PRIORITY 8

/* Initial heap capacity guess */
#define N_GUESS 100

#define ALLOTMENT_AT_LEVEL(level) (QUANTUM * (1 << level))

/* include lib header files that you need here: */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include <stdint.h> 

typedef uint worker_t;

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    TERMINATED
};

typedef struct TCB {
    worker_t thread_id;                    // Increasing thread ID
    enum thread_state thread_status;       // Using enum instead of mapping it to int values
    ucontext_t context;
    int priority;                          // Priority to be entered in the scheduling queue
    void * stack;                          // Stack pointer
    size_t stack_size;                     // Stack size
    void *return_value;                    // Return value of the thread
    
    // Time statistics
    struct timeval creation_time;
    struct timeval first_run_time;
    struct timeval completion_time;
    int has_run;
    
    // PSJF fields
    int elapsed;                           // Number of quantums thread has run
    
    // CFS fields
    uint64_t vruntime;                     // Virtual runtime for CFS
	// how many quantums has this thread run for
	//more elapsed means it will run for a longer time (assumption from project pdf)

	//MLFQ fields
	int time_at_current_level;     // Milliseconds used at current priority level
    int allotment_at_level;        // Total ms allowed at current level
    struct timeval last_scheduled;  // When thread was last scheduled
    int was_preempted;             // 1 if preempted by timer, 0 if yielded

    void *(*start_routine)(void *);
    void *arg;
    
} tcb;

typedef struct queue_node {
    tcb *thread;
    struct queue_node *next;
} queue_node;

/* mutex struct definition */
typedef struct worker_mutex_t {
    int initialized;                       // 0 = not initialized, 1 = initialized
    int locked;                            // 0 = unlocked, 1 = locked
    worker_t *owner_thread;                // Current Worker thread holding the mutex
    queue_node *waiting_queue;             // Queue of threads waiting for the mutex
} worker_mutex_t;

/* Heap node for CFS min-heap */
typedef struct heap_node{
    uint64_t vruntime;
    tcb* th;
} heapnode;

/* Min-heap structure for CFS */
typedef struct Heap{
    heapnode* arr;
    int size;
    int capacity;
} Heap;

/* Function Declarations: */

/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, void
    *(*function)(void*), void * arg);

/* give CPU possession to other user level worker threads voluntarily */
int worker_yield();

/* queue operations for PSJF and MLFQ */
void enqueue(tcb* new_tcb, int priority);
tcb* dequeue();
tcb* dequeue_min_elapsed();

/* heap operations for CFS */
void heap_init(Heap *heap, int capacity);
void heap_free(Heap* heap);
void heap_insert(Heap *heap, tcb *thread);
heapnode heap_pop(Heap* heap);
void heap_sift_down(Heap* heap, int i);

/* terminate a thread */
void worker_exit(void *value_ptr);

/* wait for thread termination */
int worker_join(worker_t thread, void **value_ptr);

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t
    *mutexattr);

/* acquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex);

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex);

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex);

/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void);

#ifdef USE_WORKERS
#define pthread_t worker_t
#define pthread_mutex_t worker_mutex_t
#define pthread_create worker_create
#define pthread_exit worker_exit
#define pthread_join worker_join
#define pthread_mutex_init worker_mutex_init
#define pthread_mutex_lock worker_mutex_lock
#define pthread_mutex_unlock worker_mutex_unlock
#define pthread_mutex_destroy worker_mutex_destroy
#endif

#endif