// Copyright Vladislav Alenik, 2026

// Feature test macro.
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>
#include <stdatomic.h>

//----------------------------
// Параметры тестового стенда
//----------------------------

#define NUM_WRITERS 4U
#define NUM_READERS 4U
#define NUM_THREADS ((NUM_WRITERS) + (NUM_READERS))
#define NUM_READER_HW_THREADS 4U
#define NUM_WRITER_HW_THREADS 4U
#define NUM_HW_THREADS ((NUM_READER_HW_THREADS) + (NUM_WRITER_HW_THREADS))

#define WRITER_BACKOFF_NANOSECONDS 1000U
#define NUM_WRITES 100000ULL
#define ONE_INCREMENT 10000000ULL

#define NUM_READS 10000000ULL

#define CHECK_CORRECTNESS 1U

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct {
    size_t thread_i;
    pthread_mutex_t* wrmutex;
    volatile uint32_t* seqlock;
    volatile uint32_t* low;
    volatile uint32_t* high;
    uint64_t copy;
} THREAD_ARGS;

void* thread_writer(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    printf("I am thread#%zu (writer)\n", args->thread_i);

    for (size_t i = 0U; i < NUM_WRITES; ++i)
    {
        // Захватываем критическую секцию как писатель.
        int ret = pthread_mutex_lock(args->wrmutex);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to acquire writer lock\n");
            exit(EXIT_FAILURE);
        }

        atomic_fetch_add_explicit(args->seqlock, 1U, memory_order_acquire);

        uint32_t low  = atomic_load_explicit(args->low,  memory_order_relaxed);
        uint64_t high = atomic_load_explicit(args->high, memory_order_relaxed);

        uint64_t current = (high << 32U) | low;

        // Делаем запись в переменную, охраняемую критической секцией.
        current += ONE_INCREMENT;

        *args->high = current >> 32U;
        *args->low  = current & 0xFFFFFFFFU;

        // Освобождаем критическую секцию как писатель.
        atomic_fetch_add_explicit(args->seqlock, 1U, memory_order_release);

        // Освобождаем критическую секцию как писатель.
        ret = pthread_mutex_unlock(args->wrmutex);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to release writer lock\n");
            exit(EXIT_FAILURE);
        }

        // Производим ожидание, т.к. писатели делают редкие записи.
        struct timespec time_to_sleep = {
            .tv_sec  = 0,
            .tv_nsec = WRITER_BACKOFF_NANOSECONDS
        };
        nanosleep(&time_to_sleep, NULL);
    }

    return NULL;
}

void* thread_reader(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    printf("I am thread#%zu (reader)\n", args->thread_i);

    for (size_t i = 0U; i < NUM_READS; ++i)
    {
        uint32_t seq0;
        uint32_t seq1;

        uint32_t low;
        uint64_t high;
        do
        {
            // Выполняем первое чтение seqlock-а. 
            seq0 = atomic_load_explicit(args->seqlock, memory_order_acquire);
            
            low  = *args->low;
            high = *args->high;

            // Выполняем второе чтение seqlock-а.
            atomic_thread_fence(memory_order_acquire);
            seq1 = atomic_fetch_add_explicit(args->seqlock, 0U, memory_order_relaxed);
        }
        while (seq0 != seq1 || seq0 & 1U);

        args->copy = (high << 32U) | low;
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
    // Переменная, которую инкрементируют все писатели.
    volatile uint32_t low  = 0;
    volatile uint32_t high = 0;

    // Инициализируем объект синхронизации.
    pthread_mutex_t wrmutex = PTHREAD_MUTEX_INITIALIZER;
    volatile uint32_t seqlock = 0;

    // Инициализируем параметры потоков.
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].wrmutex  = &wrmutex;
        args[i].seqlock  = &seqlock;
        args[i].low      = &low;
        args[i].high     = &high;
        args[i].copy     = 0U;
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


        if (i < NUM_WRITERS)
        {   // Поток является писателем.
            size_t hart_i = i % NUM_WRITER_HW_THREADS;
            CPU_SET(hart_i, &assigned_harts);
        }
        else
        {   // Поток является читателем.
            size_t hart_i = NUM_WRITER_HW_THREADS + ((i - NUM_WRITERS) % NUM_READER_HW_THREADS);
            CPU_SET(hart_i, &assigned_harts);
        }

        // Устанавливаем аффинность потока.
        ret = pthread_attr_setaffinity_np(&thread_attributes, sizeof(cpu_set_t), &assigned_harts);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to call pthread_attr_setaffinity_np\n");
            exit(EXIT_FAILURE);
        }

        // Создаём потоки POSIX.
        if (i < NUM_WRITERS)
        {   // Поток является писателем.
            ret = pthread_create(&thread_info[i].tid, &thread_attributes, thread_writer, &args[i]);
        }
        else
        {   // Поток является читателем.
            ret = pthread_create(&thread_info[i].tid, &thread_attributes, thread_reader, &args[i]);
        }
        if (ret != 0)
        {
            fprintf(stderr, "Unable to create thread\n");
            exit(EXIT_FAILURE);
        }

        // Удаляем объект с аттрибутами потока.
        pthread_attr_destroy(&thread_attributes);
    }

    // Ждём, пока читатели закончат выполнение.
    for (size_t i = NUM_WRITERS; i < NUM_THREADS; ++i)
    {
        int ret = pthread_join(thread_info[i].tid, NULL);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to join thread\n");
            exit(EXIT_FAILURE);
        }
    }

#if CHECK_CORRECTNESS
    // Ждём, пока писатели закончат выполнение.
    for (size_t i = 0; i < NUM_WRITERS; ++i)
    {
        int ret = pthread_join(thread_info[i].tid, NULL);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to join thread\n");
            exit(EXIT_FAILURE);
        }
    }

    // Выводим результат вычисления.
    uint64_t result = (((uint64_t) high) << 32U) | low;

    printf("Result of the computation: %lu\n", result);
#endif

    return EXIT_SUCCESS;
}
