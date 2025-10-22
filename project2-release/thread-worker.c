// File:	thread-worker.c
// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"
static void schedule();
static void setup_timer();
static void on_tick();
//Global counter for total context switches and 
//average turn around and response time
long tot_cntx_switches=0;
//double avg_turn_time=0;
//double avg_resp_time=0;
//DEPRECATED

static int total_threads_created = 0;
static int total_threads_completed = 0;
static double total_turnaround_time = 0;
static double total_response_time = 0;

int came_from_timer;
int quantums_since_reset = 0;

// INITAILIZE ALL YOUR OTHER VARIABLES HERE
// YOUR CODE HERE
#define STACK_SIZE SIGSTKSZ
#define NUM_PRIORITIES 10
static worker_t next_thread_id = 1;
static queue_node *priority_queues[NUM_PRIORITIES] = {NULL};
static ucontext_t scheduler_context;
static ucontext_t main_context;
static void *scheduler_stack;
static int scheduler_initialized = 0;
static tcb *current_thread = NULL;
static tcb *main_tcb = NULL;
static queue_node *all_threads_list = NULL;

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
		printf("scheduler context getcontext complete\n");
		void *stack_ptr=malloc(STACK_SIZE);
		if (!stack_ptr) {
			perror("malloc(stack)");
			exit(1);
		}
		scheduler_context.uc_stack.ss_sp=stack_ptr;
		scheduler_context.uc_stack.ss_size=STACK_SIZE;
		scheduler_context.uc_link=NULL;
		main_tcb = (tcb *)malloc(sizeof(tcb));
		main_tcb->thread_id = 0;
		main_tcb->thread_status = THREAD_READY;
		main_tcb->priority = 0;
		main_tcb->stack = NULL;
		main_tcb->stack_size=0;

		//timestamping
		gettimeofday(&main_tcb->creation_time, NULL);
		main_tcb->has_run = 0;  // Mark as not yet run
		total_threads_created++;

		// For PSJF
		main_tcb->elapsed = 0;   

		//Main TCB setup
		getcontext(&main_tcb->context);
		queue_node *mainnode = malloc(sizeof(queue_node));
		mainnode->thread=main_tcb;
		mainnode->next=NULL;
		all_threads_list=mainnode;
		
		makecontext(&scheduler_context,schedule,0);
		scheduler_initialized = 1;
		setup_timer();
	   }

	   tcb *new_tcb = (tcb *)malloc(sizeof(tcb));
	   if(!new_tcb) return -1;

	   new_tcb->thread_id=next_thread_id++;
	   new_tcb->thread_status= THREAD_READY;
	   new_tcb->priority = 0;

	   // Initialize timing fields
	   gettimeofday(&new_tcb->creation_time, NULL);
	   new_tcb->has_run = 0;
	   new_tcb->elapsed = 0;
	   total_threads_created++;

	   //Allocate Stack
	   new_tcb->stack = malloc(STACK_SIZE);
	   if(!new_tcb->stack){
		free(new_tcb);
        return -1;
	   }
	   new_tcb->stack_size = STACK_SIZE;

	   if (getcontext(&new_tcb->context) < 0) {
		printf("getcontext failed\n");
        free(new_tcb->stack);
        free(new_tcb);
        return -1;
    }

	new_tcb->context.uc_stack.ss_sp = new_tcb->stack;
    new_tcb->context.uc_stack.ss_size = STACK_SIZE;
    new_tcb->context.uc_link = &scheduler_context;
	queue_node *node = malloc(sizeof(queue_node));
	node->thread=new_tcb;
	node->next=NULL;

	//adding to all threads linked list:
	queue_node *tmp = all_threads_list;
	while(tmp->next!=NULL){
		tmp=tmp->next;
	}
	tmp->next=node;
	printf("context setup complete\n");

	makecontext(&new_tcb->context, (void (*)())function, 1, arg);
	printf("makecontext complete\n");
	*thread = new_tcb->thread_id;

	enqueue(new_tcb, 0); // switch to tcb->priority for part 2
	// Don't start scheduler here - let worker_join start it
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
	for (int i = 0; i < NUM_PRIORITIES; i++) {
        if (priority_queues[i] != NULL) {
            queue_node *temp = priority_queues[i];
			tcb *popped = temp->thread;
            priority_queues[i] = priority_queues[i]->next;
			free(temp);
            return popped;
        }
    }
	// printf("dequeue failed\n");
    return NULL;
}

//Straightforward implementation of dequeue_min
// Run through priorities (until one is non empty) and find thread w shortest elapsed and pop ts
tcb* dequeue_min_elapsed() {
    tcb *min_thread = NULL;
    queue_node *min_prev = NULL, *min_node = NULL;
    int min_elapsed = 999999999; //set to a large number
    
    // Search all pq for thread with min elapsed quantum
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        queue_node *prev = NULL;
        queue_node *curr = priority_queues[i];
        
        while (curr != NULL) {
            if (curr->thread->thread_status == THREAD_READY && 
                curr->thread->elapsed < min_elapsed) {
                min_elapsed = curr->thread->elapsed;
                min_thread = curr->thread;
                min_node = curr;
                min_prev = prev;
                // Remember which queue this came from
                min_thread->priority = i;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    
    // Remove the minimum thread from its queue
    if (min_node != NULL) {
        int priority = min_thread->priority;
        if (min_prev == NULL) {
            priority_queues[priority] = min_node->next;
        } else {
            min_prev->next = min_node->next;
        }
        free(min_node);
    }
    
    return min_thread;
}


void setup_timer(){
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler=on_tick;
	sigaction(SIGPROF,&sa,NULL);

	struct itimerval ta;
	ta.it_interval.tv_sec = 0;
	ta.it_interval.tv_usec = QUANTUM * 1000; // 10ms = 10,000 microseconds
	ta.it_value.tv_sec = 0;
	ta.it_value.tv_usec = QUANTUM * 1000;
	setitimer(ITIMER_PROF, &ta, NULL);
}

void on_tick(){
	came_from_timer = 1;
    fflush(stdout);
	if (current_thread != NULL) {
		// Timer interrupt - just yield to scheduler
		// Scheduler will handle re-enqueuing and incrementing counters
		swapcontext(&current_thread->context, &scheduler_context);
	}
}

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield() {
	came_from_timer = 0;
	// - change worker thread's state from Running to Ready
	// - save context of this thread to its thread control block
	// - switch from thread context to scheduler context
	// Note: Scheduler will handle re-enqueuing
	swapcontext(&current_thread->context, &scheduler_context);
	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr) {
	// - de-allocate any dynamic memory created when starting this thread
	// YOUR CODE HERE
	if (value_ptr != NULL) {
        current_thread->return_value = value_ptr;
    }

	//Timestamp stuff
	gettimeofday(&current_thread->completion_time, NULL);
	total_threads_completed++;
	double turnaround = (current_thread->completion_time.tv_sec - 
		current_thread->creation_time.tv_sec) * 1000000.0 +
	   (current_thread->completion_time.tv_usec - 
		current_thread->creation_time.tv_usec);

	
	double response = (current_thread->first_run_time.tv_sec - 
		current_thread->creation_time.tv_sec) * 1000000.0 +
		(current_thread->first_run_time.tv_usec - 
		current_thread->creation_time.tv_usec);

	total_turnaround_time += turnaround;
	total_response_time += response;

	current_thread->thread_status = TERMINATED;
	// Don't free here - worker_join will clean up
	// Just mark as terminated and switch back to scheduler
    current_thread = NULL;
    // Scheduler will handle context switch counting
    setcontext(&scheduler_context);
};


/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr) {
	// printf("[JOIN] ENTERED worker_join for thread %d\n", thread);
    fflush(stdout);
    if(current_thread == NULL){
        swapcontext(&main_tcb->context, &scheduler_context);
    }

	// Check if thread is trying to join itself
	if(current_thread != NULL && thread == current_thread->thread_id) return -1;
    
    queue_node *tmp = all_threads_list;
    queue_node *prev = NULL;
    tcb *target_thread = NULL;
    queue_node *target_node = NULL;
    
    while(tmp != NULL){
        if(tmp->thread->thread_id == thread){
            target_thread = tmp->thread;
            target_node = tmp;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    
    if(target_thread == NULL){
		// printf("[JOIN] Thread %d not found!\n", thread);
        return -1;
    }
	
    // printf("[JOIN] Waiting for thread %d (status=%d)\n", thread, target_thread->thread_status);
    while(target_thread->thread_status != TERMINATED){
        worker_yield();
    }
    
    if(value_ptr != NULL){
        *value_ptr = target_thread->return_value;
    }
    
    free(target_thread->stack);
    free(target_thread);
    
    // Remove from list
    if(prev == NULL){
        all_threads_list = target_node->next;
    } else {
        prev->next = target_node->next;
    }
    free(target_node);
    
    return 0;
}
/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, 
                          const pthread_mutexattr_t *mutexattr) {
	//- initialize data structures for this mutex

	// YOUR CODE HERE

	mutex->locked = 0;
	mutex->initialized = 1;
	mutex->owner_thread = NULL;
	mutex->waiting_queue = NULL;

	// printf("Mutex initialized\n");
	return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex) {

        // - use the built-in test-and-set atomic function to test the mutex
        // - if the mutex is acquired successfully, enter the critical section
        // - if acquiring mutex fails, push current thread into block list and
        // context switch to the scheduler thread

        // YOUR CODE HERE

		if (mutex == NULL || !mutex->initialized) {
			// printf("Mutex not initialized\n");
			return -1;
		}

		// Disable timer interrupts while modifying mutex
		// This prevents race conditions
		sigset_t sigset, oldset;
		sigemptyset(&sigset);
		sigaddset(&sigset, SIGPROF);
		sigprocmask(SIG_BLOCK, &sigset, &oldset);

		while (mutex->locked && (mutex->owner_thread == NULL || *mutex->owner_thread != current_thread->thread_id)) {
			//Mutex is locked, add current thread to waiting queue
			// printf("Mutex is locked, thread %d is blocking\n", current_thread->thread_id);
			current_thread->thread_status = THREAD_BLOCKED;
			// Add to waiting queue
			queue_node *new_node = malloc(sizeof(queue_node));
			new_node->thread = current_thread;
			new_node->next = NULL;
			if (mutex->waiting_queue == NULL) {
				mutex->waiting_queue = new_node;
			} else {
				queue_node *curr = mutex->waiting_queue;
				while (curr->next != NULL) {
					curr = curr->next;
				}
				curr->next = new_node;
			}
			// printf("Successfully added to waiting queue\n");
			        // Re-enable signals before switching out
			
			
			sigprocmask(SIG_SETMASK, &oldset, NULL);
	
			//swap context as per instructions
			// Save current thread context and switch to scheduler
			swapcontext(&current_thread->context, &scheduler_context);
			
			// When we return, re-block signals and check again
			sigprocmask(SIG_BLOCK, &sigset, &oldset);
		}

		// Acquired the mutex now
		mutex->locked = 1;
		mutex->owner_thread = &current_thread->thread_id;
		// printf("Thread %d acquired the mutex\n", current_thread->thread_id);

		// Re-enable timer interrupts
		sigprocmask(SIG_SETMASK, &oldset, NULL);

		
        return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex) {
	// - release mutex and make it available again. 
	// - put threads in block list to run queue 
	// so that they could compete for mutex later.

	if (mutex == NULL || !mutex->initialized) {
		// printf("Mutex not initialized\n");
		return -1;
	}

	if (mutex->owner_thread == NULL || *mutex->owner_thread != current_thread->thread_id) {
		// printf("Thread %d does not own the mutex\n", current_thread->thread_id);
		return -1;
	}

	// Disable interrupts
    sigset_t sigset, oldset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPROF);
    sigprocmask(SIG_BLOCK, &sigset, &oldset);

	// Wake up one thread from the waiting queue if any
	if (mutex->waiting_queue != NULL) {
		queue_node *node_to_wake = mutex->waiting_queue;
		mutex->waiting_queue = mutex->waiting_queue->next;
		tcb *thread_to_wake = node_to_wake->thread;
		thread_to_wake->thread_status = THREAD_READY;
		enqueue(thread_to_wake, thread_to_wake->priority);
		free(node_to_wake);
		// printf("Woke up thread %d from waiting queue\n", thread_to_wake->thread_id);
	}

	mutex->locked = 0;
	mutex->owner_thread = NULL;

	// Re-enable interrupts
    sigprocmask(SIG_SETMASK, &oldset, NULL);

	// YOUR CODE HERE
	return 0;
};


/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex) {
	// - de-allocate dynamic memory created in worker_mutex_init

	if (mutex == NULL || !mutex->initialized) {
		// printf("Mutex not initialized\n");
		return -1;
	}
	if (mutex->locked) {
		// printf("Cannot destroy a locked mutex\n");
		return -1;
	}

	// Free waiting queue
	queue_node *curr = mutex->waiting_queue;
	while (curr != NULL) {
		queue_node *temp = curr;
		curr = curr->next;
		free(temp);
	}
	mutex->waiting_queue = NULL;
	mutex->locked = 0;
	mutex->initialized = 0;
	mutex->owner_thread = NULL;

	return 0;
};

/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
static void sched_fcfs() {
	// - your own implementation of PSJF
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE
	// printf("[SCHED] Scheduler running, current_thread=%p\n", current_thread);
	while(1) {
        tcb* next = dequeue();
        
        if(next == NULL) {
            // printf("[SCHED] No threads ready, returning to main\n");
            setcontext(&main_tcb->context);
        }
        
        // printf("[SCHED] Scheduling thread %d\n", next->thread_id);
        current_thread = next;
        current_thread->thread_status = THREAD_RUNNING;
        
        swapcontext(&scheduler_context, &current_thread->context);
        // When thread yields/exits, execution returns here

		if (current_thread && current_thread->thread_status == THREAD_RUNNING) {
            current_thread->thread_status = TERMINATED;
            tot_cntx_switches++;
        }
    }
	// 	- invoke scheduling algorithms according to the policy (PSJF or MLFQ or CFS
	// printf("first come first serve scheduling complete\n");
}

static void sched_psjf() {
    // Scheduler loop - continuously schedule threads
    while (1) {
        // If current thread was running and not terminated, put it back
        if (current_thread != NULL &&
            current_thread->thread_status != TERMINATED &&
            current_thread->thread_status != THREAD_BLOCKED) {
            
            // Increment elapsed time for the thread that just ran
            current_thread->elapsed++;
            
            // Set status to ready and re-enqueue
            current_thread->thread_status = THREAD_READY;
            enqueue(current_thread, current_thread->priority);
        }
        
        // Get thread with minimum elapsed time
        tcb *next_thread = dequeue_min_elapsed();
        
        if (next_thread == NULL) {
            // No runnable threads - return to main
            setcontext(&main_tcb->context);
        }
        
        // Track first run time
        if (!next_thread->has_run) {
            gettimeofday(&next_thread->first_run_time, NULL);
            next_thread->has_run = 1;
        }
        
        // Context switch
        tot_cntx_switches++;
        
        // Set as running
        next_thread->thread_status = THREAD_RUNNING;
        current_thread = next_thread;
        
        // Switch to selected thread
        swapcontext(&scheduler_context, &next_thread->context);
        // When thread yields/exits, execution returns here and loop continues
    }
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

	while(1) {
		// STEP 1: Handle the thread that just ran
		if (current_thread != NULL && 
		    current_thread->thread_status != THREAD_BLOCKED && 
		    current_thread->thread_status != TERMINATED) {
			
			// Rule 4: Count time at this level regardless of how CPU was given up
			// Increment elapsed whether preempted or yielded
			if (came_from_timer) {
				// Thread was preempted - used full quantum
				current_thread->elapsed++;
			} else {
				// Thread yielded voluntarily - still counts toward allotment (Rule 4)
				current_thread->elapsed++;
			}
			
			// Check if thread used up its allotment at this level
			if (current_thread->elapsed >= QUANTUMS_PER_PRIORITY) {
				// Demote to lower priority queue (Rule 4)
				if (current_thread->priority < NUM_PRIORITIES - 1) {
					current_thread->priority++;  // Move to lower priority
				}
				// Reset elapsed counter for the new level
				current_thread->elapsed = 0;
			}
			
			// Re-enqueue at current priority level
			current_thread->thread_status = THREAD_READY;
			enqueue(current_thread, current_thread->priority);
		}
		
		// STEP 2: Check for priority boost (Rule 5)
		quantums_since_reset++;
		if (quantums_since_reset >= S) {
			// Move all threads in all queues to priority 0
			// First, reset elapsed for threads already at priority 0
			queue_node *curr0 = priority_queues[0];
			while (curr0 != NULL) {
				curr0->thread->elapsed = 0;
				curr0 = curr0->next;
			}
			
			// Now move threads from lower priorities to priority 0
			for (int level = 1; level < NUM_PRIORITIES; level++) {
				queue_node *curr = priority_queues[level];
				while (curr != NULL) {
					queue_node *next_node = curr->next;
					tcb *thread = curr->thread;
					
					// Reset thread priority and elapsed time
					thread->priority = 0;
					thread->elapsed = 0;
					
					// Move to top priority queue (creates new node)
					enqueue(thread, 0);
					
					// Free the old node
					free(curr);
					
					curr = next_node;
				}
				// Clear this priority level
				priority_queues[level] = NULL;
			}
			quantums_since_reset = 0;
		}
		
		// STEP 3: Pick next thread from highest priority queue (Rule 1 & 2)
		tcb *next = dequeue();  // Already picks from highest priority!
		
		if (next == NULL) {
			// No runnable threads - return to main
			setcontext(&main_tcb->context);
		}
		
		// Track first run time for statistics
		if (!next->has_run) {
			gettimeofday(&next->first_run_time, NULL);
			next->has_run = 1;
		}
		
		// STEP 4: Context switch and run selected thread
		tot_cntx_switches++;
		next->thread_status = THREAD_RUNNING;
		current_thread = next;
		
		swapcontext(&scheduler_context, &next->context);
		// When thread yields/exits, execution returns here and loop continues
	}
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
	// printf("schedule function called\n");
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
	double avg_turn_time = total_turnaround_time / total_threads_completed;
	double avg_resp_time = total_response_time / total_threads_completed;

       fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
       fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
       fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}


// Feel free to add any other functions you need

// YOUR CODE HERE

