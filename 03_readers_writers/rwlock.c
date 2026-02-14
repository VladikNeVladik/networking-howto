// Copyright Vladislav Alenik, 2026

// Feature test macro.
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

//----------------------------
// Параметры тестового стенда
//----------------------------

#define NUM_WRITERS 4U
#define NUM_READERS 16U
#define NUM_THREADS ((NUM_WRITERS) + (NUM_READERS))
#define NUM_READER_HW_THREADS 4U
#define NUM_WRITER_HW_THREADS 4U
#define NUM_HW_THREADS ((NUM_READER_HW_THREADS) + (NUM_WRITER_HW_THREADS))

#define READER_BACKOFF_NANOSECONDS 10000U 

#define NUM_ITERATIONS 10000000ULL
#define ONE_INCREMENT  10000000ULL

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct {
    size_t thread_i;
    pthread_rwlock_t* rwlock;
    uint64_t* var;
    uint64_t copy;
} THREAD_ARGS;

void* thread_writer(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    printf("I am thread#%zu (writer)\n", args->thread_i);

    for (size_t i = 0U; i < NUM_ITERATIONS; ++i)
    {
        // Захватываем критическую секцию как писатель.
        int ret = pthread_rwlock_wrlock(args->rwlock);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to acquire writer lock\n");
            exit(EXIT_FAILURE);
        }

        *(args->var) += ONE_INCREMENT;

        // Освобождаем критическую секцию как писатель.
        ret = pthread_rwlock_unlock(args->rwlock);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to release writer lock\n");
            exit(EXIT_FAILURE);
        }
    }

    return NULL;
}


void* thread_reader(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    printf("I am thread#%zu (reader)\n", args->thread_i);

    do
    {
        // Захватываем критическую секцию как читатель.
        int ret = pthread_rwlock_rdlock(args->rwlock);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to acquire reader lock\n");
            exit(EXIT_FAILURE);
        }

        args->copy = *args->var;

        // Освобождаем критическую секцию как читатель.
        ret = pthread_rwlock_unlock(args->rwlock);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to release reader lock\n");
            exit(EXIT_FAILURE);
        }

        // Производим ожидание, т.к. читателю данные нужны не всегда.
        struct timespec time_to_sleep = {
            .tv_sec  = 0,
            .tv_nsec = READER_BACKOFF_NANOSECONDS
        };
        nanosleep(&time_to_sleep, NULL);

    } while (args->copy != NUM_WRITERS * NUM_ITERATIONS * ONE_INCREMENT);

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
    uint64_t var = 0U;

    // Инициализируем объект синхронизации.
    pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

    // Инициализируем параметры потоков.
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].rwlock   = &rwlock;
        args[i].var      = &var;
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
    printf("Result of the computation: %lu\n", var);

    for (size_t i = NUM_WRITERS; i < NUM_THREADS; ++i)
    {
        printf("Thread #%zu (reader) copy: %lu\n", i, args[i].copy);
    }

    int ret = pthread_rwlock_destroy(&rwlock);
    if (ret != 0)
    {
        fprintf(stderr, "Unable to destroy reader-writer lock\n");
        exit(EXIT_FAILURE);
    }

    return EXIT_SUCCESS;
}
