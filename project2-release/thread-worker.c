//netid: ssm229 Saroop Makhija
//netid: mk2177 Aiman Koli
//ilab: cd.cs.rutgers.edu
#include "thread-worker.h"
static void schedule();
static void setup_timer();
static void on_tick();
//Global counter for total context switches and 
//average turn around and response time
long tot_cntx_switches=0;

static int total_threads_created = 0;
static int total_threads_completed = 0;
static double total_turnaround_time = 0;
static double total_response_time = 0;
// static int cleanup_done = 0;
int came_from_timer;
int quantums_since_reset = 0;

// INITAILIZE ALL YOUR OTHER VARIABLES HERE
#define STACK_SIZE SIGSTKSZ
#define N_GUESS 100
static worker_t next_thread_id = 1;
static queue_node *priority_queues[NUM_PRIORITIES] = {NULL};
static ucontext_t scheduler_context;
static ucontext_t main_context;
static void *scheduler_stack;
static int scheduler_initialized = 0;
static tcb *current_thread = NULL;
static tcb *main_tcb = NULL;
static queue_node *all_threads_list = NULL;
static Heap cfs_heap;
static int timeslice_ms;
static sigset_t profmask;
static inline void block_prof(void){ 
	sigemptyset(&profmask); 
	sigaddset(&profmask, SIGPROF); 
	sigprocmask(SIG_BLOCK, &profmask, NULL); 
}
static inline void unblock_prof(void){ sigprocmask(SIG_UNBLOCK, &profmask, NULL); }

// CFS specific variables
static struct timeval cfs_thread_start_time;
static struct itimerval cfs_timer;

// Forward declarations for CFS helper functions
static tcb* heap_extract_min(Heap *heap);
static void set_cfs_timer(int microseconds);
static uint64_t get_elapsed_time_us(struct timeval *start);
//func
static void thread_wrapper(uintptr_t tcb_word);

static int cfs_fixed_slice_ms = -1;    // computed once
static int cfs_initial_threads = -1;   // snapshot of initial worker count


/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, 
                      void *(*function)(void*), void * arg) {

       if(!scheduler_initialized){
		getcontext(&scheduler_context);
		// printf("scheduler context getcontext complete\n");
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

		// Initialize vruntime for main thread
		main_tcb->vruntime = 0;
		
		//cfs stuff
		heap_init(&cfs_heap, N_GUESS);

		//timestamping
		gettimeofday(&main_tcb->creation_time, NULL);
		main_tcb->has_run = 0;
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
	   new_tcb->first_run_time.tv_sec = 0;  
	   new_tcb->first_run_time.tv_usec = 0;  

	   //for psjf
	   new_tcb->elapsed = 0;
	   
	   total_threads_created++;

	    // MLFQ fields initialization
		new_tcb->time_at_current_level = 0;
		new_tcb->allotment_at_level = ALLOTMENT_AT_LEVEL(0);
		new_tcb->last_scheduled.tv_sec = 0;
		new_tcb->last_scheduled.tv_usec = 0;
		new_tcb->was_preempted = 0;
	   
	   // cfs field
	   new_tcb->vruntime = 0;

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

	//changes 
	new_tcb->start_routine = function;
    new_tcb->arg = arg;
	
	queue_node *node = malloc(sizeof(queue_node));
	node->thread=new_tcb;
	node->next=NULL;
	

	//adding to all threads linked list:
	queue_node *tmp = all_threads_list;
	while(tmp->next!=NULL){
		tmp=tmp->next;
	}
	tmp->next=node;

	// makecontext(&new_tcb->context, (void (*)())function, 1, arg);
	makecontext(&new_tcb->context, (void (*)())thread_wrapper, 1, (uintptr_t)new_tcb);
	*thread = new_tcb->thread_id;

	#if defined(CFS)
    heap_insert(&cfs_heap, new_tcb);
	#elif defined(MLFQ)
		enqueue(new_tcb, 0);
	#elif defined(PSJF)
		enqueue(new_tcb, 0);
	#endif

	// if (next_thread_id == 1) {
	// 	swapcontext(&main_context, &scheduler_context);
	// }
    return 0;
};
static void thread_wrapper(uintptr_t tcb_word) {
    tcb *self = (tcb *)(uintptr_t)tcb_word;
    void *ret = self->start_routine(self->arg);
    worker_exit(ret); // never returns
}

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
    return NULL;
}

tcb* dequeue_min_elapsed() {
    tcb *min_thread = NULL;
    queue_node *min_prev = NULL, *min_node = NULL;
    int min_elapsed = 999999999;
    
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
                min_thread->priority = i;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    
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



void heap_init(Heap *heap, int capacity) {
	heap->arr = (heapnode *)malloc(sizeof(heapnode) * capacity);
	heap->size = 0;
	heap->capacity = capacity;
}

void heap_free(Heap* heap){
	free(heap->arr);
	heap->arr = NULL;
	heap->size = 0;
	heap->capacity = 0;
}

void heap_insert(Heap *heap, tcb *thread) {
    if (!heap || !thread) return;

    if (heap->size == heap->capacity) {
        int newcap = heap->capacity ? heap->capacity * 2 : 1;
        heapnode *tmp = (heapnode *)realloc(heap->arr, newcap * sizeof(*tmp));
        if (!tmp) return;
        heap->arr = tmp;
        heap->capacity = newcap;
    }

    int i = heap->size;
    heap->arr[i].vruntime = thread->vruntime;
    heap->arr[i].th = thread;
    heap->size++;

    while (i > 0) {
        int p = (i - 1) / 2;

        int child_less =
            (heap->arr[i].vruntime < heap->arr[p].vruntime) ||
            (heap->arr[i].vruntime == heap->arr[p].vruntime &&
             heap->arr[i].th->thread_id < heap->arr[p].th->thread_id);

        if (!child_less) break;

        heapnode tmp = heap->arr[i];
        heap->arr[i] = heap->arr[p];
        heap->arr[p] = tmp;
        i = p;
    }
}

static inline int less_node(const heapnode *a, const heapnode *b) {
    if (a->vruntime != b->vruntime) return a->vruntime < b->vruntime;
    if (a->th && b->th) return a->th->thread_id < b->th->thread_id;
    return a->th != NULL;
}

void heap_sift_down(Heap* heap, int i){
	for(;;){
		int left = (2 * i) +1;
		int right = (2*i) +2;
		int smallest =i;

		if(left < heap->size && less_node(&heap->arr[left],&heap->arr[smallest])){
			smallest =  left;
		}
		if(right < heap->size && less_node(&heap->arr[right], &heap->arr[smallest])){
			smallest = right;
		}

		if(smallest == i) break;

		heapnode tmp = heap->arr[i];
        heap->arr[i] = heap->arr[smallest];
        heap->arr[smallest] = tmp;

		i = smallest;
	}
}

heapnode heap_pop(Heap* heap){
	heapnode none = {0, NULL};
	if(!heap || heap->size == 0) return none;

	heapnode root = heap->arr[0];

	heap->size--;
	if (heap->size > 0) {
        heap->arr[0] = heap->arr[heap->size];
        heap_sift_down(heap, 0);
    }
	
	return root;

}

// Helper function to extract minimum thread from heap
static tcb* heap_extract_min(Heap *heap) {
    heapnode min_node = heap_pop(heap);
    return min_node.th;
}

// Helper function to get elapsed time in microseconds
static uint64_t get_elapsed_time_us(struct timeval *start) {
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t elapsed = (now.tv_sec - start->tv_sec) * 1000000ULL + 
                       (now.tv_usec - start->tv_usec);
    return elapsed;
}

// Set variable timer for CFS
static void set_cfs_timer(int microseconds) {
    cfs_timer.it_interval.tv_sec = 0;
    cfs_timer.it_interval.tv_usec = 0; // One-shot timer
    cfs_timer.it_value.tv_sec = microseconds / 1000000;
    cfs_timer.it_value.tv_usec = microseconds % 1000000;
    setitimer(ITIMER_PROF, &cfs_timer, NULL);
}

void setup_timer(){
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler=on_tick;
	sigaction(SIGPROF,&sa,NULL);

	// For PSJF and MLFQ, use fixed quantum
	#if !defined(CFS)
	struct itimerval ta;
	ta.it_interval.tv_sec = 0;
	ta.it_interval.tv_usec = QUANTUM * 1000;
	ta.it_value.tv_sec = 0;
	ta.it_value.tv_usec = QUANTUM * 1000;
	setitimer(ITIMER_PROF, &ta, NULL);
	#endif
	// CFS will set timer dynamically in sched_cfs
}

void on_tick(){
	came_from_timer = 1;
	if (current_thread != NULL) {
		swapcontext(&current_thread->context, &scheduler_context);
	}
}

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield() {
	block_prof();
	came_from_timer = 0;
	swapcontext(&current_thread->context, &scheduler_context);
	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr) {
	block_prof();
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
    current_thread = NULL;    
    setcontext(&scheduler_context);
};


/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr) {
    if(current_thread == NULL){
        swapcontext(&main_tcb->context, &scheduler_context);
    }

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
        return -1;
    }
	
    while(target_thread->thread_status != TERMINATED){
        worker_yield();
    }
    
    if(value_ptr != NULL){
        *value_ptr = target_thread->return_value;
    }
    
    if (target_thread->stack) {
        free(target_thread->stack);  // Free thread's stack
    }
    free(target_thread);  // Free TCB
    
    // Remove from all_threads_list and free the list node
    if(prev == NULL){
        all_threads_list = target_node->next;
    } else {
        prev->next = target_node->next;
    }
    free(target_node);  // Free the queue_node from all_threads_list
    
    return 0;
}

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, 
                          const pthread_mutexattr_t *mutexattr) {
	mutex->locked = 0;
	mutex->initialized = 1;
	mutex->owner_thread = NULL;
	mutex->waiting_queue = NULL;

	return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex) {
	if (mutex == NULL || !mutex->initialized) {
		return -1;
	}

	sigset_t sigset, oldset;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGPROF);
	sigprocmask(SIG_BLOCK, &sigset, &oldset);

	while (mutex->locked && (mutex->owner_thread == NULL || *mutex->owner_thread != current_thread->thread_id)) {
		current_thread->thread_status = THREAD_BLOCKED;
	
		queue_node *new_node = malloc(sizeof(queue_node));
		new_node->thread = current_thread;
		new_node->next = NULL;
		if (mutex->waiting_queue == NULL) {
			mutex->waiting_queue = new_node;
		} else {
			queue_node *curr = mutex->waiting_queue;
			while (curr->next != NULL) curr = curr->next;
			curr->next = new_node;
		}
	
		swapcontext(&current_thread->context, &scheduler_context);
	}
	mutex->locked = 1;
	mutex->owner_thread = &current_thread->thread_id;
	sigprocmask(SIG_SETMASK, &oldset, NULL);
	return 0;	

};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex) {
	if (mutex == NULL || !mutex->initialized) {
		return -1;
	}

	if (mutex->owner_thread == NULL || *mutex->owner_thread != current_thread->thread_id) {
		return -1;
	}

    sigset_t sigset, oldset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPROF);
    sigprocmask(SIG_BLOCK, &sigset, &oldset);

	if (mutex->waiting_queue != NULL) {
		queue_node *node_to_wake = mutex->waiting_queue;
		mutex->waiting_queue = mutex->waiting_queue->next;
		tcb *thread_to_wake = node_to_wake->thread;
		thread_to_wake->thread_status = THREAD_READY;
		
		// For CFS, insert back into heap
		#if defined(CFS)
		heap_insert(&cfs_heap, thread_to_wake);
		#else
		enqueue(thread_to_wake, thread_to_wake->priority);
		#endif
		
		free(node_to_wake);
	}

	mutex->locked = 0;
	mutex->owner_thread = NULL;

    sigprocmask(SIG_SETMASK, &oldset, NULL);

	return 0;
};


/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex) {
	if (mutex == NULL || !mutex->initialized) {
		return -1;
	}
	if (mutex->locked) {
		return -1;
	}

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

static void boost_all_threads() {
    // Move all threads to highest priority (0)
    for (int i = 1; i < NUM_PRIORITIES; i++) {
        while (priority_queues[i] != NULL) {
            queue_node *node = priority_queues[i];
            priority_queues[i] = node->next;
            tcb *thread = node->thread;
            
            // Reset thread's priority and time tracking
            thread->priority = 0;
            thread->time_at_current_level = 0;
            thread->allotment_at_level = ALLOTMENT_AT_LEVEL(0);
            
            // Re-enqueue at highest priority
            node->next = NULL;
            if (priority_queues[0] == NULL) {
                priority_queues[0] = node;
            } else {
                queue_node *temp = priority_queues[0];
                while (temp->next != NULL) temp = temp->next;
                temp->next = node;
            }
        }
    }
}

/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
static void sched_fcfs() {
	while(1) {
        tcb* next = dequeue();
        
        if(next == NULL) {
            setcontext(&main_tcb->context);
        }
        
        current_thread = next;
        current_thread->thread_status = THREAD_RUNNING;
        
        swapcontext(&scheduler_context, &current_thread->context);

		if (current_thread && current_thread->thread_status == THREAD_RUNNING) {
            current_thread->thread_status = TERMINATED;
            tot_cntx_switches++;
        }
    }
}

static void sched_psjf() {
    while (1) {
		block_prof();
        if (current_thread != NULL &&
            current_thread->thread_status != TERMINATED &&
            current_thread->thread_status != THREAD_BLOCKED) {
            
            current_thread->elapsed++;
            current_thread->thread_status = THREAD_READY;
            enqueue(current_thread, current_thread->priority);
        }
        
        tcb *next_thread = dequeue_min_elapsed();
        
        if (next_thread == NULL) {
			unblock_prof();
            setcontext(&main_tcb->context);
        }
        
        if (!next_thread->has_run) {
            gettimeofday(&next_thread->first_run_time, NULL);
            next_thread->has_run = 1;
        }
        
        tot_cntx_switches++;
        
        next_thread->thread_status = THREAD_RUNNING;
        current_thread = next_thread;
        unblock_prof();
        swapcontext(&scheduler_context, &next_thread->context);
    }
}

/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq_deprecated() {
	while(1) {
		if (current_thread != NULL && 
		    current_thread->thread_status != THREAD_BLOCKED && 
		    current_thread->thread_status != TERMINATED) {
			
			if (came_from_timer) {
				current_thread->elapsed++;
			} else {
				current_thread->elapsed++;
			}
			
			if (current_thread->elapsed >= QUANTUMS_PER_PRIORITY) {
				if (current_thread->priority < NUM_PRIORITIES - 1) {
					current_thread->priority++;
				}
				current_thread->elapsed = 0;
			}
			
			current_thread->thread_status = THREAD_READY;
			enqueue(current_thread, current_thread->priority);
		}
		
		quantums_since_reset++;
		if (quantums_since_reset >= S) {
			queue_node *curr0 = priority_queues[0];
			while (curr0 != NULL) {
				curr0->thread->elapsed = 0;
				curr0 = curr0->next;
			}
			
			for (int level = 1; level < NUM_PRIORITIES; level++) {
				queue_node *curr = priority_queues[level];
				while (curr != NULL) {
					queue_node *next_node = curr->next;
					tcb *thread = curr->thread;
					
					thread->priority = 0;
					thread->elapsed = 0;
					
					enqueue(thread, 0);
					
					free(curr);
					
					curr = next_node;
				}
				priority_queues[level] = NULL;
			}
			quantums_since_reset = 0;
		}
		
		tcb *next = dequeue();
		
		if (next == NULL) {
			setcontext(&main_tcb->context);
		}
		
		if (!next->has_run) {
			gettimeofday(&next->first_run_time, NULL);
			next->has_run = 1;
		}
		
		tot_cntx_switches++;
		next->thread_status = THREAD_RUNNING;
		current_thread = next;
		
		swapcontext(&scheduler_context, &next->context);
	}
}


static void sched_mlfq() {
    static int total_quantums = 0;  // Track total quantums for Rule 5
    struct timeval now;
    block_prof();
    // Handle current thread that was just running
    if (current_thread != NULL && 
        current_thread->thread_status != TERMINATED &&
        current_thread->thread_status != THREAD_BLOCKED) {
        
        // Calculate how long this thread actually ran
        gettimeofday(&now, NULL);
        int runtime_ms = 0;
        if (current_thread->last_scheduled.tv_sec > 0) {
            runtime_ms = (now.tv_sec - current_thread->last_scheduled.tv_sec) * 1000 +
                        (now.tv_usec - current_thread->last_scheduled.tv_usec) / 1000;
        }
        
        // Add to time at current level
        current_thread->time_at_current_level += runtime_ms;
        
        // Check if we came from timer (ran full quantum) or yield
        current_thread->was_preempted = (runtime_ms >= QUANTUM - 1); // Allow 1ms tolerance
        
        // Rule 4: Check if allotment exhausted at this level
        if (current_thread->time_at_current_level >= current_thread->allotment_at_level) {
            // Demote to next lower priority (higher number)
            if (current_thread->priority < NUM_PRIORITIES - 1) {
                current_thread->priority++;
                current_thread->time_at_current_level = 0; // Reset time at new level
                current_thread->allotment_at_level = ALLOTMENT_AT_LEVEL(current_thread->priority);
            }
        }
        
        // Set back to ready and enqueue
        current_thread->thread_status = THREAD_READY;
        enqueue(current_thread, current_thread->priority);
    }
    
    // Rule 5: Priority boost after S quantums
    total_quantums++;
    if (total_quantums >= S) {
        boost_all_threads();
        total_quantums = 0;
    }
    
    // Get next thread from highest priority queue
    tcb *next_thread = dequeue();
    
    if (next_thread == NULL) {
        // No runnable threads
		unblock_prof();
        if (current_thread == NULL) {
            setcontext(&main_tcb->context);
        }
        return;
    }
    
    // Track first run time
    if (!next_thread->has_run) {
        gettimeofday(&next_thread->first_run_time, NULL);
        next_thread->has_run = 1;
    }
    
    // Record when we're scheduling this thread
    gettimeofday(&next_thread->last_scheduled, NULL);
    
    // Context switch
    if (current_thread != next_thread) {
        tot_cntx_switches++;
    }
    
    // Set as running
    next_thread->thread_status = THREAD_RUNNING;
    current_thread = next_thread;
    
    // Switch to selected thread
	unblock_prof();
    setcontext(&next_thread->context);
}

/* Completely fair scheduling algorithm */
static void sched_cfs_dep(){
	while(1){
		// Step 1: Update vruntime of previously running thread
		if (current_thread != NULL &&
		    current_thread->thread_status != TERMINATED &&
		    current_thread->thread_status != THREAD_BLOCKED) {
			
			// Calculate actual runtime in microseconds
			uint64_t runtime_us = get_elapsed_time_us(&cfs_thread_start_time);
			
			// Convert to milliseconds for vruntime (to keep numbers manageable)
			uint64_t runtime_ms = runtime_us / 1000;
			
			// Update vruntime
			current_thread->vruntime += runtime_ms;
			
			// Set thread back to ready and insert into heap
			current_thread->thread_status = THREAD_READY;
			heap_insert(&cfs_heap, current_thread);
		}
		
		// Step 2: Get thread with minimum vruntime
		tcb *next_thread = heap_extract_min(&cfs_heap);
		
		if (next_thread == NULL) {
			// No runnable threads
			setcontext(&main_tcb->context);
		}
		
		// Step 3: Calculate time slice
		int num_threads = cfs_heap.size + 1; // +1 for the thread we just extracted
		if (num_threads < 1) num_threads = 1; // Safety check
		
		timeslice_ms = TARGET_LATENCY / num_threads;
		
		// Step 4: Enforce minimum granularity
		if (timeslice_ms < MIN_SCHED_GRN) {
			timeslice_ms = MIN_SCHED_GRN;
		}
		
		// Step 5: Set timer for this time slice (convert to microseconds)
		set_cfs_timer(timeslice_ms * 1000);
		
		// Step 6: Track statistics
		if (!next_thread->has_run) {
			gettimeofday(&next_thread->first_run_time, NULL);
			next_thread->has_run = 1;
		}
		
		// Step 7: Record start time for runtime calculation
		gettimeofday(&cfs_thread_start_time, NULL);
		
		// Step 8: Context switch
		tot_cntx_switches++;
		next_thread->thread_status = THREAD_RUNNING;
		current_thread = next_thread;
		
		swapcontext(&scheduler_context, &next_thread->context);
		// Execution returns here when thread yields/exits/preempted
	}
}


static void sched_cfs() {
    while (1) {
		block_prof();
        /* Step 1: Account the previously running thread and reinsert if still runnable */
        if (current_thread != NULL &&
            current_thread->thread_status != TERMINATED &&
            current_thread->thread_status != THREAD_BLOCKED) {

            uint64_t runtime_us = get_elapsed_time_us(&cfs_thread_start_time);
            uint64_t runtime_ms = runtime_us / 1000;        /* keep vruntime in ms */
            current_thread->vruntime += runtime_ms;

            current_thread->thread_status = THREAD_READY;
            heap_insert(&cfs_heap, current_thread);
        }

        /* Step 2: Pick the thread with minimum vruntime */
        tcb *next_thread = heap_extract_min(&cfs_heap);
        if (next_thread == NULL) {
            /* No runnable threads: return to main */
			unblock_prof();
            setcontext(&main_tcb->context);
        }

        /* Step 3: Compute fixed timeslice once, based on initial worker count (exclude main) */
        if (cfs_fixed_slice_ms < 0) {
            cfs_initial_threads = total_threads_created - 1;
            if (cfs_initial_threads < 1) cfs_initial_threads = 1;

            int ts = TARGET_LATENCY / cfs_initial_threads;
            if (ts < MIN_SCHED_GRN) ts = MIN_SCHED_GRN;
            cfs_fixed_slice_ms = ts;
        }

        /* Step 4: Arm a one-shot timer with the fixed slice */
        set_cfs_timer(cfs_fixed_slice_ms * 1000);  /* ms -> us */

        /* Step 5: First-run timestamp for response-time metric */
        if (!next_thread->has_run) {
            gettimeofday(&next_thread->first_run_time, NULL);
            next_thread->has_run = 1;
        }

        /* Step 6: Record dispatch start to measure actual runtime on return */
        gettimeofday(&cfs_thread_start_time, NULL);

        /* Step 7: Context switch */
        tot_cntx_switches++;
        next_thread->thread_status = THREAD_RUNNING;
        current_thread = next_thread;
		unblock_prof();
        swapcontext(&scheduler_context, &next_thread->context);
        /* Execution resumes here when the running thread yields/exits/is preempted */
    }
}



/* scheduler */
static void schedule() {
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