// No copyright. Vladislav Alenik, 2024
// Modified by me

// Feature test macro:
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h> // syscall
#include <linux/futex.h> // FUTEX_* macros

// CPU_SET macros:
#include <sched.h>
// Threads:
#include <pthread.h>

//----------------------
// Benchmark parameters
//----------------------

#define NUM_THREADS 4U
#define NUM_HARDWARE_THREAD 4U

const size_t NUM_ITERATIONS = 10000000U;

//------------------
// Thread execution
//------------------

typedef struct {
    size_t thread_i;
    int* mutex;
} THREAD_ARGS;

// Variable to race on:
uint32_t var = 0U;

// states of mutex
enum {
    M_ULOCKD = 0,
    M_LOCKD = 1,
    M_LOCKD_WQ = 2
};

// futex_syscall wrapper
static int futex(int *uaddr, int futex_op, int val,
        const struct timespec *timeout, int *uaddr2, int val3) {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

// mutex lock function
void lock(int *mutex) {
    int c;

    if ((c = __sync_val_compare_and_swap(mutex, M_ULOCKD, M_LOCKD)) != M_ULOCKD) {
        if (c != M_LOCKD_WQ) {
            c = __atomic_exchange_n(mutex, M_LOCKD_WQ, __ATOMIC_ACQ_REL);
        }

        while (c != M_ULOCKD) {
            futex(mutex, FUTEX_WAIT, M_LOCKD_WQ, NULL, NULL, 0);
            c = __atomic_exchange_n(mutex, M_LOCKD_WQ, __ATOMIC_ACQ_REL);
        }
    }
}

// mutex unlock
void unlock(int *mutex) {
    // atomic decrement returning old value of variable
    if (__atomic_fetch_sub(mutex, (int)M_LOCKD, __ATOMIC_ACQ_REL) != M_LOCKD) {
        *mutex = M_ULOCKD;
        futex(mutex, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;
    printf("I am thread#%zu\n", args->thread_i);

    for (size_t i = 0U; i < NUM_ITERATIONS; ++i)
    {
        // Basic critical section among the threads:
        lock(args->mutex);
        var++;
        unlock(args->mutex);
    }

    return NULL;
}

//------------------
// Thread benchmark
//------------------

typedef struct {
    pthread_t tid;
} THREAD_INFO;

int main() {
    // Initialize mutual exclusion object:
    // NOTE: by default, use fast mutexes.
    _Alignas(64) int mutex_var = 0;

    // Initialize thread data:
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].mutex    = &mutex_var;
    }

    // Spawn threads:
    THREAD_INFO thread_info[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        // Initialize thread attributes:
        pthread_attr_t thread_attributes;
        int ret = pthread_attr_init(&thread_attributes);
        if (ret != 0) {
            fprintf(stderr, "Unable to call pthread_attr_init\n");
            exit(EXIT_FAILURE);
        }

        // Assign hardware thread to posix thread:
        cpu_set_t assigned_harts;
        CPU_ZERO(&assigned_harts);

        // Assumptions:
        // - There are NUM_HARDWARE_THREAD hardware threads.
        // - All harts from 0 to are present.
        size_t hart_i = i % NUM_HARDWARE_THREAD;
        CPU_SET(hart_i, &assigned_harts);

        // Set thread affinity:
        ret = pthread_attr_setaffinity_np(&thread_attributes, sizeof(cpu_set_t), &assigned_harts);
        if (ret != 0) {
            fprintf(stderr, "Unable to call pthread_attr_setaffinity_np\n");
            exit(EXIT_FAILURE);
        }

        // Create POSIX thread:
        ret = pthread_create(&thread_info[i].tid, &thread_attributes, thread_func, &args[i]);
        if (ret != 0) {
            fprintf(stderr, "Unable to create thread\n");
            exit(EXIT_FAILURE);
        }

        // Destroy thread attribute object:
        pthread_attr_destroy(&thread_attributes);
    }

    // Wait for all threads to finish execution:
    for (size_t i = 0; i < NUM_THREADS; ++i) {
        int ret = pthread_join(thread_info[i].tid, NULL);

        if (ret != 0) {
            fprintf(stderr, "Unable to join thread\n");
            exit(EXIT_FAILURE);
        }
    }

    // Print incremented variable:
    printf("Result of the computation: %u\n", var);

    return EXIT_SUCCESS;
}
