// Copyright Vladislav Alenik, 2024

// Feature test macro.
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>

//----------------------------
// Параметры тестового стенда
//----------------------------

#define NUM_THREADS 8U
#define NUM_HARDWARE_THREAD 8U

const size_t NUM_ITERATIONS = 10000000U;

//------------------------------------------------------------------
// TAS lock
//------------------------------------------------------------------
// Оптимизации:
// - Инструкция x86 "pause" для энергоэффективного ожидания.
// - Стратегия "exponential backoff".
//------------------------------------------------------------------

typedef struct
{
    atomic_flag lock_taken;
} TAS_Lock;

const unsigned TAS_CYCLES_TO_SPIN          =    10;
const unsigned TAS_MIN_BACKOFF_NANOSECONDS =  1000;
const unsigned TAS_MAX_BACKOFF_NANOSECONDS = 64000;

void TAS_init(TAS_Lock* lock)
{
    atomic_flag_clear_explicit(&lock->lock_taken, memory_order_release);
}

void TAS_acquire(TAS_Lock* lock)
{
    unsigned backoff_sleep = TAS_MIN_BACKOFF_NANOSECONDS;

    // Perform exponential backoff:
    while (atomic_flag_test_and_set_explicit(&lock->lock_taken, memory_order_acquire))
    {
        struct timespec to_sleep = {
            .tv_sec  = 0,
            .tv_nsec = backoff_sleep + (rand() % TAS_MIN_BACKOFF_NANOSECONDS)
        };

        if (backoff_sleep < TAS_MAX_BACKOFF_NANOSECONDS) backoff_sleep *= 2;

        // No value-checking, because it doesn't affect correctness:
        nanosleep(&to_sleep, NULL);
    }
}

void TAS_release(TAS_Lock* lock)
{
    atomic_flag_clear_explicit(&lock->lock_taken, memory_order_release);
}

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct {
    size_t thread_i;
    TAS_Lock* spinlock;
} THREAD_ARGS;

// Переменная, которую инкрементируют все потоки.
uint32_t var = 0U;

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    printf("I am thread#%zu\n", args->thread_i);

    for (size_t i = 0U; i < NUM_ITERATIONS; ++i)
    {
        // Создание критической секции с помощью spinlock-а.
        TAS_acquire(args->spinlock);

        var++;

        TAS_release(args->spinlock);
    }

    return NULL;
}

//--------------------------------
// Инициализация тестового стенда
//--------------------------------

typedef struct {
    pthread_t tid;
} THREAD_INFO;

int main()
{
    // Инициализируем объект синхронизации.
    TAS_Lock spinlock;
    TAS_init(&spinlock);

    // Инициализируем параметры потоков.
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].spinlock = &spinlock;
    }

    // Запуск потоков.
    THREAD_INFO thread_info[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        // Инициализируем аттрибуты потока.
        pthread_attr_t thread_attributes;
        int ret = pthread_attr_init(&thread_attributes);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to call pthread_attr_init\n");
            exit(EXIT_FAILURE);
        }

        // Назначаем аппаратные потоки для потоков POSIX.
        cpu_set_t assigned_harts;
        CPU_ZERO(&assigned_harts);

        // Предположения о системе:
        // - Система имеет NUM_HARDWARE_THREAD аппаратных потоков.
        //   Это число возможно извлекать из системы напрямую
        // - Все аппаратные потоки с 0 по NUM_HARDWARE_THREAD-1 активны.
        //   Это требование может нарушаться при выходе из строя какого-нибудь из ядер процессора.
        size_t hart_i = i % NUM_HARDWARE_THREAD;
        CPU_SET(hart_i, &assigned_harts);

        // Устанавливаем аффинность потока.
        ret = pthread_attr_setaffinity_np(&thread_attributes, sizeof(cpu_set_t), &assigned_harts);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to call pthread_attr_setaffinity_np\n");
            exit(EXIT_FAILURE);
        }

        // Создаём потоки POSIX.
        ret = pthread_create(&thread_info[i].tid, &thread_attributes, thread_func, &args[i]);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to create thread\n");
            exit(EXIT_FAILURE);
        }

        // Удаляем объект с аттрибутами потока.
        pthread_attr_destroy(&thread_attributes);
    }

    // Ждём, пока все потоки закончат выполнение.
    for (size_t i = 0; i < NUM_THREADS; ++i)
    {
        int ret = pthread_join(thread_info[i].tid, NULL);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to join thread\n");
            exit(EXIT_FAILURE);
        }
    }

    // Выводим результат вычисления.
    printf("Result of the computation: %u\n", var);

    return EXIT_SUCCESS;
}
