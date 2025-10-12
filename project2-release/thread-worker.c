// File:	thread-worker.c
// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"

//Global counter for total context switches and 
//average turn around and response time
long tot_cntx_switches=0;
double avg_turn_time=0;
double avg_resp_time=0;


// INITAILIZE ALL YOUR OTHER VARIABLES HERE
// YOUR CODE HERE
#define STACK_SIZE 8192
static worker_t next_thread_id = 0;
static queue_node ready_queue;
static ucontext_t scheduler_context;
static int scheduler_initialized = 0;

/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, 
                      void *(*function)(void*), void * arg) {

       // - create Thread Control Block (TCB)
       // - create and initialize the context of this worker thread
       // - allocate space of stack for this thread to run
       // after everything is set, push this thread into run queue and 
       // - make it ready for the execution.

       // YOUR CODE HERE
	   if(!scheduler_initialized){
		getcontext(&scheduler_context);
		*stack_ptr=malloc(STACK_SIZE);
		if (!*stack_ptr) {
			perror("malloc(stack)");
			exit(1);
		}
		schdulder_context->uc_stack.ss_sp=stack_ptr;
		schdulder_context->uc_stack.ss_size=STACK_SIZE;
		schdulder_context->uc_link=NULL;
		makecontext(&schdulder_context,schedule,0)
		scheduler_initialized = 1;
	   }

	   tcb *new_tcb = (tcb *)malloc(sizeof(tcb));
	   if(!new_tcb) return -1;

	   new_tcb->thread_id=next_thread_id++;
	   new_tcb->thread_status= THREAD_READY;
	   new_tcb->priority = 0;

	   //Allocate Stack
	   new_tcb->stack = malloc(STACK_SIZE);
	   if(!new_tcb->stack){
		free(new_tcb);
        return -1;
	   }
	   new_tcb->stack_size = STACK_SIZE;

	   if (getcontext(&new_tcb->context) < 0) {
        free(new_tcb->stack);
        free(new_tcb);
        return -1;
    }

	new_tcb->context.uc_stack.ss_sp = new_tcb->stack;
    new_tcb->context.uc_stack.ss_size = STACK_SIZE;
    new_tcb->context.uc_link = NULL;

	makecontext(&new_tcb->context, (void (*)())function, 1, arg);
	*thread = new_tcb->thread_id;

	enqueue(new_tcb, 0); // switch to tcb->priority for part 2
	
    return 0;
};

void enqueue(tcb* new_tcb, int priority) {
    queue_node *new_node = malloc(sizeof(queue_node));
    new_node->thread = new_tcb;
    new_node->next = NULL;
    
    if (priority_queues[priority] == NULL) {
        priority_queues[priority] = new_node;
    } else {
        queue_node *curr = priority_queues[priority];
        while (curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = new_node;
    }
}

tcb* dequeue() {
    if (ready_queue == NULL) return NULL;
    
    queue_node *temp = ready_queue;
    tcb *popped = temp->thread;
    ready_queue = ready_queue->next;
    free(temp);
    
    return popped;
}

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield() {
	
	// - change worker thread's state from Running to Ready
	// - save context of this thread to its thread control block
	// - switch from thread context to scheduler context

	// YOUR CODE HERE
	
	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr) {
	// - de-allocate any dynamic memory created when starting this thread

	// YOUR CODE HERE
};


/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr) {
	
	// - wait for a specific thread to terminate
	// - de-allocate any dynamic memory created by the joining thread
  
	// YOUR CODE HERE
	return 0;
};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, 
                          const pthread_mutexattr_t *mutexattr) {
	//- initialize data structures for this mutex

	// YOUR CODE HERE
	return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex) {

        // - use the built-in test-and-set atomic function to test the mutex
        // - if the mutex is acquired successfully, enter the critical section
        // - if acquiring mutex fails, push current thread into block list and
        // context switch to the scheduler thread

        // YOUR CODE HERE
        return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex) {
	// - release mutex and make it available again. 
	// - put threads in block list to run queue 
	// so that they could compete for mutex later.

	// YOUR CODE HERE
	return 0;
};


/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex) {
	// - de-allocate dynamic memory created in worker_mutex_init

	return 0;
};

/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
static void sched_psjf() {
	// - your own implementation of PSJF
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE
}


/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq() {
	// - your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE

	/* Step-by-step guidances */
	// Step1: Calculate the time current thread actually ran
	// Step2.1: If current thread uses up its allotment, demote it to the low priority queue (Rule 4)
	// Step2.2: Otherwise, push the thread back to its origin queue
	// Step3: If time period S passes, promote all threads to the topmost queue (Rule 5)
	// Step4: Apply RR on the topmost queue with entries and run next thread
}

/* Completely fair scheduling algorithm */
static void sched_cfs(){
	// - your own implementation of CFS
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE

	/* Step-by-step guidances */

	// Step1: Update current thread's vruntime by adding the time it actually ran
	// Step2: Insert current thread into the runqueue (min heap)
	// Step3: Pop the runqueue to get the thread with a minimum vruntime
	// Step4: Calculate time slice based on target_latency (TARGET_LATENCY), number of threads within the runqueue
	// Step5: If the ideal time slice is smaller than minimum_granularity (MIN_SCHED_GRN), use MIN_SCHED_GRN instead
	// Step5: Setup next time interrupt based on the time slice
	// Step6: Run the selected thread
}


/* scheduler */
static void schedule() {
	// - every time a timer interrupt occurs, your worker thread library 
	// should be contexted switched from a thread context to this 
	// schedule() function
	
	//YOUR CODE HERE

	// - invoke scheduling algorithms according to the policy (PSJF or MLFQ or CFS)
#if defined(PSJF)
    	sched_psjf();
#elif defined(MLFQ)
	sched_mlfq();
#elif defined(CFS)
    	sched_cfs();  
#else
	# error "Define one of PSJF, MLFQ, or CFS when compiling. e.g. make SCHED=MLFQ"
#endif
}



//DO NOT MODIFY THIS FUNCTION
/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void) {

       fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
       fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
       fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}


// Feel free to add any other functions you need

// YOUR CODE HERE

