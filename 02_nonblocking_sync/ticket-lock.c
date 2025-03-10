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

const size_t NUM_ITERATIONS = 1000U;

//------------------------------------------------------------------
// Ticket lock
//------------------------------------------------------------------
// Оптимизации:
// - FIFO fairness
// - Инструкция x86 "pause" для энергоэффективного ожидания.
// - Переходим к следующему потоку при взятой блокировке.
//------------------------------------------------------------------

#define spinloop_pause() __asm__ volatile("pause")

typedef struct
{
    _Atomic uint16_t next_ticket;
    _Atomic uint16_t now_serving;
} TicketLock;

const unsigned TICKET_CYCLES_TO_SPIN = 100;

void TicketLock_init(TicketLock* lock)
{
    atomic_store_explicit(&lock->next_ticket, 0, memory_order_relaxed);
    atomic_store_explicit(&lock->now_serving, 0, memory_order_release);
}

void TicketLock_acquire(TicketLock* lock)
{
    // Acquire a ticket in a queue:
    const short ticket = atomic_fetch_add_explicit(&lock->next_ticket, 1U, memory_order_relaxed);

    // On start spin-loop waiting for the lock to be released:
    for (unsigned cycle_no = 0; lock->now_serving != ticket && cycle_no < TICKET_CYCLES_TO_SPIN; ++cycle_no)
    {
        spinloop_pause();
    }

    while (atomic_load_explicit(&lock->now_serving, memory_order_acquire) != ticket)
    {
        sched_yield();
    }
}

void TicketLock_release(TicketLock* lock)
{
    atomic_fetch_add_explicit(&lock->now_serving, 1, memory_order_release);
}

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct {
    size_t thread_i;
    TicketLock* spinlock;
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
        TicketLock_acquire(args->spinlock);

        var++;

        TicketLock_release(args->spinlock);
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
    TicketLock spinlock;
    TicketLock_init(&spinlock);

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
