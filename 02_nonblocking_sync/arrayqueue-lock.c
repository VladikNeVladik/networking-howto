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
#define CACHE_LINE_SIZE 64U

const size_t NUM_ITERATIONS = 10000000U;

//------------------------------------------------------------------
// Array-based queue lock
//------------------------------------------------------------------
// Оптимизации:
// - Инструкция x86 "pause" для энергоэффективного ожидания.
// - Каждый поток ожидает только на одной линии кеша.
//------------------------------------------------------------------

#define spinloop_pause() __asm__ volatile("pause")

typedef struct
{
    // Флаг доступности критической секции.
    _Atomic uint8_t available;
    uint8_t padding[CACHE_LINE_SIZE - 1];
} __attribute__((aligned(CACHE_LINE_SIZE), packed)) QueueNode;

_Static_assert(
    sizeof(QueueNode) == CACHE_LINE_SIZE,
    "Invalid QueueNode size\n");

typedef struct
{
    QueueNode* nodes;
    uint16_t size;
    _Atomic uint16_t head;
    pthread_key_t indexKey;
} ArrayQueue_Lock;

const uint32_t ARRAYQUEUE_CYCLES_TO_YEILD = 10000;

void AQL_destroy_perthread(void* memory);

void AQL_init(ArrayQueue_Lock* lock, uint16_t size)
{
    lock->size = size;
    lock->nodes = calloc(size, sizeof(QueueNode));

    atomic_store_explicit(&lock->nodes[0].available, 1, memory_order_relaxed);

    for (uint16_t i = 1; i < size; ++i)
    {
        atomic_store_explicit(&lock->nodes[i].available, 0, memory_order_relaxed);
    }

    atomic_store_explicit(&lock->head, 0, memory_order_release);

    int ret = pthread_key_create(&lock->indexKey, AQL_destroy_perthread);
    if (ret != 0)
    {
        printf("Unable to allocate key for dynamic thread-local storage\n");
        exit(EXIT_FAILURE);
    }
}

void AQL_destroy(ArrayQueue_Lock* lock)
{
    int ret = pthread_key_delete(lock->indexKey);
    if (ret != 0)
    {
        printf("Unable to release key for dynamic thread-local storage\n");
        exit(EXIT_FAILURE);
    }
}

void AQL_init_perthread(ArrayQueue_Lock* lock)
{
    void* memory = calloc(1U, sizeof(uint16_t));
    if (memory == NULL)
    {
        printf("Unable to allocate dynamic thread-local storage\n");
        exit(EXIT_FAILURE);
    }

    int ret = pthread_setspecific(lock->indexKey, memory);
    if (ret != 0)
    {
        printf("Unable to set dynamic thread-local storage\n");
        exit(EXIT_FAILURE);
    }
}

void AQL_destroy_perthread(void* memory)
{
    if (memory != NULL)
    {
        free(memory);
    }
}

void AQL_acquire(ArrayQueue_Lock* lock)
{
    uint16_t index = atomic_fetch_add_explicit(&lock->head, 1U, memory_order_relaxed) % lock->size;

    uint16_t* indexPtr = (uint16_t*) pthread_getspecific(lock->indexKey);
    *indexPtr = index;

    uint32_t iteration = 0U;
    while (atomic_load_explicit(&lock->nodes[index].available, memory_order_acquire) == 0)
    {
        spinloop_pause();

        if (iteration == ARRAYQUEUE_CYCLES_TO_YEILD)
        {
            iteration = 0U;
            pthread_yield();
        }
        iteration++;
    }
}

void AQL_release(ArrayQueue_Lock* lock)
{
    uint16_t* indexPtr = (uint16_t*) pthread_getspecific(lock->indexKey);
    uint16_t index = *indexPtr;

    atomic_store_explicit(
        &lock->nodes[index].available, 0, memory_order_relaxed);
    atomic_store_explicit(
        &lock->nodes[(index + 1) % lock->size].available, 1, memory_order_release);
}

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct {
    size_t thread_i;
    ArrayQueue_Lock* spinlock;
} THREAD_ARGS;

// Переменная, которую инкрементируют все потоки.
uint32_t var = 0U;

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    printf("I am thread#%zu\n", args->thread_i);

    AQL_init_perthread(args->spinlock);

    for (size_t i = 0U; i < NUM_ITERATIONS; ++i)
    {
        // Создание критической секции с помощью spinlock-а.
        AQL_acquire(args->spinlock);

        var++;

        AQL_release(args->spinlock);
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
    ArrayQueue_Lock spinlock;
    AQL_init(&spinlock, NUM_THREADS);

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

    AQL_destroy(&spinlock);

    // Выводим результат вычисления.
    printf("Result of the computation: %u\n", var);

    return EXIT_SUCCESS;
}
