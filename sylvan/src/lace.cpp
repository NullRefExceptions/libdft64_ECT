/*
 * Copyright 2013-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "pin.H"
#include <errno.h> // for errno
#include <sched.h> // for sched_getaffinity
#include <stdio.h>  // for fprintf
#include <stdlib.h> // for memalign, malloc
#include <string.h> // for memset
#include <sys/time.h> // for gettimeofday
#include "sylvan_tls.h"

#include "lace.h"

#include "sylvan_int.h"
using namespace sylvan;

#if LACE_USE_MMAP
#include <sys/mman.h> // for mmap, etc
#endif

/**
 * (public) Worker data
 */
static Worker **workers = NULL;

/**
 * Default sizes for program stack and task deque
 */
static size_t stacksize = 0; // 0 means just take default
static size_t default_dqsize = 100000;

/**
 * 因为barriar的存在，我们不能仅使用lace_n_workers，否则
 * 提前结束的worker将导致其他worker永远阻塞在barriar上
 */
unsigned int lace_n_workers_alive = 0;
unsigned int lace_n_workers_id = 0;
PIN_SEMAPHORE sem_helpers;
uint64_t helper_count =0;
bool helper_inited = false;
int __attribute__((aligned(LINE_SIZE))) helper_quit = 0;

/**
 * Datastructure of the task deque etc for each worker.
 * - first public cachelines (accessible via global "workers" variable)
 * - then private cachelines
 * - then the deque array
 */
typedef struct {
    Worker worker_public;
    char pad1[PAD(sizeof(Worker), LINE_SIZE)];
    WorkerP worker_private;
    char pad2[PAD(sizeof(WorkerP), LINE_SIZE)];
    Task deque[];
} worker_data;

/**
 * (Secret) holds pointers to the memory block allocated for each worker
 */
static worker_data **workers_memory = NULL;

/**
 * Number of bytes allocated for each worker's worker data.
 */
static size_t workers_memory_size = 0;

/**
 * (Secret) holds pointer to private Worker data, just for stats collection at end
 */
static WorkerP **workers_p;

static SSA_Task *ssa_p;//与外部通信
static sylvan_tcb_t *lace_worker_tls;

static uint64_t spawn_exit_lock;

/**
 * Thread-specific mechanism to access current worker data
 */
DECLARE_THREAD_LOCAL(current_worker,WorkerP *);

/**
 * Global newframe variable used for the implementation of NEWFRAME and TOGETHER
 */
lace_newframe_t lace_newframe;

/**
 * Get the private Worker data of the current thread
 */
WorkerP*
lace_get_worker()
{
    LOCALIZE_THREAD_LOCAL(current_worker, WorkerP *);
    return current_worker;
}

/**
 * Find the head of the task deque, using the given private Worker data
 */
Task*
lace_get_head(WorkerP *self)
{
    Task *dq = self->dq;

    /* First check the first tasks linearly */
    if (dq[0].thief == 0) return dq;
    if (dq[1].thief == 0) return dq+1;
    if (dq[2].thief == 0) return dq+2;
    if (dq[3].thief == 0) return dq+3;

    /* Then fast search for a low/high bound using powers of 2: 4, 8, 16... */
    size_t low = 2;
    size_t high = self->end - self->dq;

    for (;;) {
        if (low*2 >= high) {
            break;
        } else if (dq[low*2].thief == 0) {
            high=low*2;
            break;
        } else {
            low*=2;
        }
    }

    /* Finally zoom in using binary search */
    while (low < high) {
        size_t mid = low + (high-low)/2;
        if (dq[mid].thief == 0) high = mid;
        else low = mid + 1;
    }

    return dq+low;
}

/**
 * Get the default stack size (or 0 for automatically determine)
 */
size_t
lace_get_stacksize()
{
    return stacksize;
}

void
lace_set_stacksize(size_t new_stacksize)
{
    stacksize = new_stacksize;
}

/**
 * Lace barrier implementation, that synchronizes on all currently enabled workers.
 */
typedef struct {
    volatile int __attribute__((aligned(LINE_SIZE))) count;
    volatile int __attribute__((aligned(LINE_SIZE))) leaving;
    volatile int __attribute__((aligned(LINE_SIZE))) wait;
} barrier_t;

barrier_t lace_bar;

/**
 * Enter the Lace barrier and wait until all workers have entered the Lace barrier.
 */
void
lace_barrier()
{
    int wait = lace_bar.wait;
    if ((int)lace_n_workers_alive == __sync_add_and_fetch(&lace_bar.count, 1)) {
        lace_bar.count = 0;
        lace_bar.leaving = lace_n_workers_alive;
        lace_bar.wait = 1 - wait; // flip wait
    } else {
        while (wait == lace_bar.wait) {} // wait
    }

    __sync_add_and_fetch(&lace_bar.leaving, -1);
}

/**
 * Initialize the Lace barrier
 */
static void
lace_barrier_init()
{
    memset(&lace_bar, 0, sizeof(barrier_t));
}

/**
 * Destroy the Lace barrier (just wait until all are exited)
 */
static void
lace_barrier_destroy()
{
    // wait for all to exit
    while (lace_bar.leaving != 0) continue;
}

void
lace_init_worker(unsigned int worker)
{
    // Allocate our memory
#if LACE_USE_MMAP
    workers_memory[worker] = (worker_data *)mmap(NULL, workers_memory_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (workers_memory[worker] == MAP_FAILED) {
        fprintf(stderr, "Lace error: Unable to allocate memory for the Lace worker!\n");
        exit(1);
    }
#else
    if (posix_memalign((void**)&workers_memory[worker], LINE_SIZE, workers_memory_size) != 0) {
        fprintf(stderr, "Lace error: Unable to allocate memory for the Lace worker!\n");
        exit(1);
    }
    memset(workers_memory[worker], 0, workers_memory_size);
#endif

    // Set pointers
    Worker *wt = workers[worker] = &workers_memory[worker]->worker_public;
    WorkerP *w = workers_p[worker] = &workers_memory[worker]->worker_private;
    w->dq = workers_memory[worker]->deque;

    SET_THREAD_LOCAL(current_worker,w);

    // Initialize public worker data
    wt->dq = w->dq;
    wt->ts.v = 0;
    wt->allstolen = 0;
    wt->movesplit = 0;

    // Initialize private worker data
    w->_public = wt;
    w->end = w->dq + default_dqsize;
    w->split = w->dq;
    w->allstolen = 0;
    w->worker = worker;
    w->pu = -1;
    w->enabled = 1;
    w->rng = (((uint64_t)rand())<<32 | rand());

}


/**
 * Simple random number generated (like rand) using the given seed.
 * (Used for thread-specific (scalable) random number generation.
 */
static inline uint32_t
rng(uint32_t *seed, int max)
{
    uint32_t next = *seed;

    next *= 1103515245;
    next += 12345;

    *seed = next;

    return next % max;
}
/**
 * Global "external" task
 */
typedef struct _ExtTask {
    Task *task;
    //sem_t sem;
    uint64_t volatile spin_lock;
} ExtTask;

static volatile ExtTask* external_task = 0;

void
lace_run_task(Task *task)
{
    // check if we are really not in a Lace thread
    WorkerP* self = lace_get_worker();
    if (self != 0) {
        task->f(self, lace_get_head(self), task);
    } else {
        ExtTask et;
        et.task = task;
        et.task->thief = 0;
        et.spin_lock = 1;
        compiler_barrier();
        ExtTask *exp = 0;
        while (__atomic_compare_exchange_n((ExtTask**)&external_task, &exp, &et, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) != 1) ;
        while (et.spin_lock);
    }
}

static inline void
lace_steal_external(WorkerP *self, Task *dq_head) 
{
    ExtTask *stolen_task = __atomic_exchange_n((ExtTask**)&external_task, NULL, __ATOMIC_SEQ_CST);
    if (stolen_task != 0) {
        // execute task
        stolen_task->task->thief = self->_public;
        lace_time_event(self, 1);
        compiler_barrier();
        stolen_task->task->f(self, dq_head, stolen_task->task);
        compiler_barrier();
        lace_time_event(self, 2);
        compiler_barrier();
        stolen_task->task->thief = THIEF_COMPLETED;
        stolen_task->spin_lock=0;
        compiler_barrier();
        lace_time_event(self, 8);
    }
}

VOID_TASK_1(lace_steal_loop_ex, int*, quit)
{
    // Determine who I am
    const int worker_id = __lace_worker->worker;
    SSA_Task *t = &ssa_p[worker_id];
    while(*(volatile int*)quit == 0) {
#ifdef SSA_PROFILE
    uint64_t cb_count = 0;
#endif
        //mtbdd_cube
        if (t->task_type ==1)
        {
#ifdef SSA_NOGC
            t->res = mtbdd_cube_nogc(t->arg1,(uint8_t *)t->arg2,mtbdd_true);
#else
            t->res = mtbdd_cube(t->arg1,(uint8_t *)t->arg2,mtbdd_true);
#endif
            mfence();
            t->task_type = 0;
        }
        //bdd_combine
        if (t->task_type ==2)
        {
            BDD res;
#ifdef SSA_NOGC
    #if PARALLEL_COMBINE_ENABLE
            if (unlikely(mtbdd_nodecount2(t->arg1)>PARALLEL_COMBINE_THRESHOLD 
                && mtbdd_nodecount2(t->arg2)>PARALLEL_COMBINE_THRESHOLD && helper_count>0))
            {
                PIN_SemaphoreSet(&sem_helpers);
                res = CALL(sylvan_and_nogc,sylvan_not(t->arg1), sylvan_not(t->arg2), 0);
                PIN_SemaphoreClear(&sem_helpers);
            }
            else
                res = CALL(sylvan_and_nogc_alone,sylvan_not(t->arg1), sylvan_not(t->arg2), 0);
    #else
            res = CALL(sylvan_and_nogc_alone,sylvan_not(t->arg1), sylvan_not(t->arg2), 0);
    #endif
#else
    #if PARALLEL_COMBINE_ENABLE            
            if (unlikely(mtbdd_nodecount2(t->arg1)>PARALLEL_COMBINE_THRESHOLD 
                && mtbdd_nodecount2(t->arg2)>PARALLEL_COMBINE_THRESHOLD && helper_count>0))
            {
                PIN_SemaphoreSet(&sem_helpers);
                res = CALL(sylvan_and,sylvan_not(t->arg1), sylvan_not(t->arg2), 0);
                PIN_SemaphoreClear(&sem_helpers);
            }
            else
            {
        #ifdef SSA_PROFILE
                res = CALL(sylvan_and_alone_profile,sylvan_not(t->arg1), sylvan_not(t->arg2), 0,&cb_count);
        #else
                res = CALL(sylvan_and_alone,sylvan_not(t->arg1), sylvan_not(t->arg2), 0);
        #endif
            }
    #else
        #ifdef SSA_PROFILE
                res = CALL(sylvan_and_alone_profile,sylvan_not(t->arg1), sylvan_not(t->arg2), 0,&cb_count);
        #else
                res = CALL(sylvan_and_alone,sylvan_not(t->arg1), sylvan_not(t->arg2), 0);
        #endif
#endif


#endif
            t->res = sylvan_not(res);
#ifdef SSA_PROFILE
            t->l_count1 = mtbdd_nodecount(t->arg1);
            t->r_count1 = mtbdd_nodecount(t->arg2);
    #if MTBDD_NODE_COUNTING
            t->l_count2 = mtbdd_nodecount2(t->arg1);
            t->r_count2 = mtbdd_nodecount2(t->arg2);
    #endif
            t->cb_count = cb_count;
#endif
            mfence();
            t->task_type = 0;
        }

        YIELD_NEWFRAME();

        //for debug
        if (unlikely(external_task != 0)) {
            assert(false);
        }
    }

    /* 
    当准备退出时，如果无法取得spwan_exit_lock，说明有worker正在spwan或触发了gc，我们需要
    等待它们完成或配合做完gc
    */
    while (!__sync_bool_compare_and_swap(&spawn_exit_lock,0,1))
    {
        YIELD_NEWFRAME();
    }

    mtbdd_unprotect(&ssa_p[worker_id].res);
    *ssa_p[worker_id].quit = 0;
    lace_n_workers_alive--;
    mfence();
    compiler_barrier();
    spawn_exit_lock = 0;
    
}

VOID_TASK_1(lace_steal_loop_in, int*, quit)
{
    // Determine who I am
    const int worker_id = __lace_worker->worker;
    
    // Prepare self, victim
    Worker ** const self = &workers[worker_id];
    Worker ** victim = self;

    uint32_t seed = worker_id;
    int i=0;

    while(*(volatile int*)quit == 0) {
        if (lace_n_workers_alive > 1) {
            // Select victim
            if( i>0 ) {
                i--;
                victim++;
                if (victim == self) victim++;
                if (victim >= workers + lace_n_workers_id) victim = workers;
                if (victim == self) victim++;
            } else {
                i = rng(&seed, 40); // compute random i 0..40
                victim = workers + (rng(&seed, lace_n_workers_id-1) + worker_id + 1) % lace_n_workers_id;
            }
            lace_steal(__lace_worker, __lace_dq_head, *victim);
        }

        YIELD_NEWFRAME();

        //for debug
        if (unlikely(external_task != 0)) {
            assert(false);
        }
    }
}

static void
lace_worker_thread(void* arg)
{
    int worker = (int)(size_t)arg;
    sylvan_tcb_t *s_tcb = &lace_worker_tls[worker];
    memset(s_tcb,0,sizeof(sylvan_tcb_t));
    _writefsbase_u64((uint64_t)s_tcb);

    // Initialize data structures
    lace_init_worker(worker);

    // init tls structures
    mtbdd_refs_init_key();
    //make every worker recalculate region
    lace_n_workers_id++;
    lace_n_workers_alive++;
    for (int i = 0; i <= worker; i++)
    {
        lace_worker_tls[i].slots[my_region_slot]= -1;
    }

    //we are done
    mfence();
    spawn_exit_lock = 0;

    // Run the steal loop
    WorkerP *__lace_worker = lace_get_worker();
    Task *__lace_dq_head = lace_get_head(__lace_worker);
    lace_steal_loop_ex_WORK(__lace_worker, __lace_dq_head, ssa_p[worker].quit);
    return;
}

VOID_TASK_0(lace_steal_loop_he)
{
    // Determine who I am
    const int worker_id = __lace_worker->worker;
    
    // Prepare self, victim
    Worker ** const self = &workers[worker_id];
    Worker ** victim = self;

    uint32_t seed = worker_id;
    int i=0;

    while(*(volatile int*)&helper_quit == 0) {
        PIN_SemaphoreWait(&sem_helpers);
        if (lace_n_workers_alive > 1) {
            // Select victim
            if( i>0 ) {
                i--;
                victim++;
                if (victim == self) victim++;
                if (victim >= workers + lace_n_workers_id) victim = workers;
                if (victim == self) victim++;
            } else {
                i = rng(&seed, 40); // compute random i 0..40
                victim = workers + (rng(&seed, lace_n_workers_id-1) + worker_id + 1) % lace_n_workers_id;
            }
            lace_steal(__lace_worker, __lace_dq_head, *victim);
        }

        YIELD_NEWFRAME();

        //for debug
        if (unlikely(external_task != 0)) {
            assert(false);
        }
    }
    __sync_add_and_fetch(&lace_n_workers_alive, -1);
}

static void
lace_helper_thread(void* arg)
{
    int worker = (int)(size_t)arg;
    sylvan_tcb_t *s_tcb = &lace_worker_tls[worker];
    memset(s_tcb,0,sizeof(sylvan_tcb_t));
    _writefsbase_u64((uint64_t)s_tcb);

    // Initialize data structures
    lace_init_worker(worker);

    // init tls structures
    mtbdd_refs_init_key();

    // Run the steal loop
    WorkerP *__lace_worker = lace_get_worker();
    Task *__lace_dq_head = lace_get_head(__lace_worker);
    lace_steal_loop_he_WORK(__lace_worker, __lace_dq_head);
    return;
}

SSA_Task* lace_spawn_worker(void *arg)
{   
    while (!__sync_bool_compare_and_swap(&spawn_exit_lock,0,1))
    ;
    if (!helper_inited)
    {
        for (size_t i = 0; i < helper_count; i++)
        {
            PIN_SpawnInternalThread(lace_helper_thread,(void*)(size_t)lace_n_workers_id++,stacksize,NULL);
        }
        lace_n_workers_alive = lace_n_workers_id;
        helper_inited = true;
    }
    ssa_p[lace_n_workers_id].quit = (int *)arg;
    mtbdd_protect(&ssa_p[lace_n_workers_id].res);
    SSA_Task* res = &ssa_p[lace_n_workers_id];
    PIN_SpawnInternalThread(lace_worker_thread,(void*)(size_t)lace_n_workers_id,stacksize,NULL);
    return res;
}

void
lace_start(unsigned int max_workers, size_t dqsize, uint64_t helper_n)
{

    if (dqsize != 0) default_dqsize = dqsize;
    else dqsize = default_dqsize;

    // Initialize Lace barrier
    lace_barrier_init();

    // Allocate array with all workers
    if (posix_memalign((void**)&workers, LINE_SIZE, max_workers*sizeof(Worker*)) != 0 ||
        posix_memalign((void**)&workers_p, LINE_SIZE, max_workers*sizeof(WorkerP*)) != 0 ||
        posix_memalign((void**)&workers_memory, LINE_SIZE, max_workers*sizeof(worker_data*)) ||
        posix_memalign((void**)&lace_worker_tls, LINE_SIZE, max_workers*sizeof(sylvan_tcb_t))||
        posix_memalign((void**)&ssa_p, LINE_SIZE, max_workers*sizeof(SSA_Task))!= 0) {
        fprintf(stderr, "Lace error: unable to allocate memory!\n");
        exit(1);
    }

    // Compute memory size for each worker
    workers_memory_size = sizeof(worker_data) + sizeof(Task) * dqsize;

    // Prepare lace_init structure
    lace_newframe.t = NULL;

    PIN_SemaphoreInit(&sem_helpers);
    helper_count = helper_n;
}

void lace_stop()
{
    helper_quit = 1;
    PIN_SemaphoreSet(&sem_helpers);
    while (lace_n_workers_alive != 0)
        ;
    PIN_SemaphoreFini(&sem_helpers);
    
    // finally, destroy the barriers
    lace_barrier_destroy();

    for (unsigned int i=0; i<lace_n_workers_id; i++) {
#if LACE_USE_MMAP
        munmap(workers_memory[i], workers_memory_size);
#else
        free(workers_memory[i]);
#endif
    }

    free(workers);
    workers = 0;

    free(workers_p);
    workers_p = 0;

    free(workers_memory);
    workers_memory = 0;

    free(ssa_p);
    free(lace_worker_tls);
}


/* 
仅有的执行路径是
newframe(gc)-->together(reset region)
所以我们仅在newframe的时候检查和设置spawnlock
通过spawnlock，可以保证gc过程和spawn_worker过程不会重叠

在newframe环境下，为了加快gc速度，此时会启动steal功能，但仅仅steal
内部，不应该接收external task，否则可能导致回收仍然存活的BDD
*/

VOID_TASK_0(lace_steal_random)
{
    YIELD_NEWFRAME();
    if (lace_n_workers_alive > 1) {
        Worker *victim = workers[(__lace_worker->worker + 1 + rng(&__lace_worker->seed, lace_n_workers_id-1)) % lace_n_workers_id];
        lace_steal(__lace_worker, __lace_dq_head, victim);
    }
}

/**
 * Execute the given <root> task in a new frame (synchronizing with all Lace threads)
 * 1) Creates a new frame
 * 2) LACE BARRIER
 * 3) Execute the <root> task
 * 4) LACE BARRIER
 * 5) Restore the old frame
 */
void
lace_exec_in_new_frame(WorkerP *__lace_worker, Task *__lace_dq_head, Task *root)
{
    TailSplit old;
    uint8_t old_as;

    // save old tail, split, allstolen and initiate new frame
    {
        Worker *wt = __lace_worker->_public;

        old_as = wt->allstolen;
        wt->allstolen = 1;
        old.ts.split = wt->ts.ts.split;
        wt->ts.ts.split = 0;
        mfence();
        old.ts.tail = wt->ts.ts.tail;

        TailSplit ts_new;
        ts_new.ts.tail = __lace_dq_head - __lace_worker->dq;
        ts_new.ts.split = __lace_dq_head - __lace_worker->dq;
        wt->ts.v = ts_new.v;

        __lace_worker->split = __lace_dq_head;
        __lace_worker->allstolen = 1;
    }

    // wait until all workers are ready
    lace_barrier();

    // execute task
    root->f(__lace_worker, __lace_dq_head, root);
    compiler_barrier();

    // wait until all workers are back (else they may steal from previous frame)
    lace_barrier();

    // restore tail, split, allstolen
    {
        Worker *wt = __lace_worker->_public;
        wt->allstolen = old_as;
        wt->ts.v = old.v;
        __lace_worker->split = __lace_worker->dq + old.ts.split;
        __lace_worker->allstolen = old_as;
    }
}

/**
 * This method is called when there is a new frame (NEWFRAME or TOGETHER)
 * Each Lace worker executes lace_yield to execute the task in a new frame.
 */
void
lace_yield(WorkerP *__lace_worker, Task *__lace_dq_head)
{
    // make a local copy of the task
    Task _t;
    memcpy(&_t, lace_newframe.t, sizeof(Task));

    // wait until all workers have made a local copy
    lace_barrier();

    lace_exec_in_new_frame(__lace_worker, __lace_dq_head, &_t);
}

/**
 * Root task for the TOGETHER method.
 * Ensures after executing, to steal random tasks until done.
 */
VOID_TASK_2(lace_together_root, Task*, t, volatile int*, finished)
{
    // run the root task
    t->f(__lace_worker, __lace_dq_head, t);

    // signal out completion
    __atomic_add_fetch(finished, -1, __ATOMIC_SEQ_CST);

    // while threads aren't done, steal randomly
    while (*finished != 0) STEAL_RANDOM();
}

VOID_TASK_1(lace_wrap_together, Task*, task)
{
    /* synchronization integer (decrease by 1 when done...) */
    int done = lace_n_workers_alive;

    /* wrap task in lace_together_root */
    Task _t2;
    TD_lace_together_root *t2 = (TD_lace_together_root *)&_t2;
    t2->f = lace_together_root_WRAP;
    t2->thief = THIEF_TASK;
    t2->d.args.arg_1 = task;
    t2->d.args.arg_2 = &done;
    compiler_barrier();

    /* now try to be the one who sets it! */
    while (1) {
        Task *expected = 0;
        if (__atomic_compare_exchange_n(&lace_newframe.t, &expected, &_t2, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        lace_yield(__lace_worker, __lace_dq_head);
    }

    // wait until other workers have made a local copy
    lace_barrier();
    compiler_barrier();

    // reset the newframe struct
    lace_newframe.t = 0;

    lace_exec_in_new_frame(__lace_worker, __lace_dq_head, &_t2);
}

VOID_TASK_2(lace_newframe_root, Task*, t, int*, done)
{
    t->f(__lace_worker, __lace_dq_head, t);
    *done = 1;
}

VOID_TASK_1(lace_wrap_newframe, Task*, task)
{
    /* synchronization integer (set to 1 when done...) */
    int done = 0;

    /* create the lace_steal_loop task for the other workers */
    Task _s;
    TD_lace_steal_loop_in *s = (TD_lace_steal_loop_in *)&_s;
    s->f = &lace_steal_loop_in_WRAP;
    s->thief = THIEF_TASK;
    s->d.args.arg_1 = &done;
    compiler_barrier();

    /* now try to be the one who sets it! */
    while (1) {
        Task *expected = 0;
        if (__atomic_compare_exchange_n(&lace_newframe.t, &expected, &_s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        lace_yield(__lace_worker, __lace_dq_head);
    }

    // wait until other workers have made a local copy
    lace_barrier();
    compiler_barrier();

    // reset the newframe struct, then wrap and run ours
    lace_newframe.t = 0;

    /* wrap task in lace_newframe_root */
    Task _t2;
    TD_lace_newframe_root *t2 = (TD_lace_newframe_root *)&_t2;
    t2->f = lace_newframe_root_WRAP;
    t2->thief = THIEF_TASK;
    t2->d.args.arg_1 = task;
    t2->d.args.arg_2 = &done;

    lace_exec_in_new_frame(__lace_worker, __lace_dq_head, &_t2);
}

void
lace_run_together(Task *t)
{
    WorkerP* self = lace_get_worker();
    if (self != 0) {
        lace_wrap_together_CALL(self, lace_get_head(self), t);
    } else {
        RUN(lace_wrap_together, t);
    }
}

void
lace_run_newframe(Task *t)
{
    while (!__sync_bool_compare_and_swap(&spawn_exit_lock,0,1))
    ;
    bool n = false;
    if (!PIN_SemaphoreIsSet(&sem_helpers))
    {
        PIN_SemaphoreSet(&sem_helpers);
        n = true;
    }
    
    WorkerP* self = lace_get_worker();
    if (self != 0) {
        lace_wrap_newframe_CALL(self, lace_get_head(self), t);
    } else {
        RUN(lace_wrap_newframe, t);
    }
    if (n)
    {
        PIN_SemaphoreClear(&sem_helpers);
    }
    spawn_exit_lock = 0;
}

/**
 * Called by _SPAWN functions when the Task stack is full.
 */
void
lace_abort_stack_overflow(void)
{
    fprintf(stderr, "Lace fatal error: Task stack overflow! Aborting.\n");
    exit(-1);
}
