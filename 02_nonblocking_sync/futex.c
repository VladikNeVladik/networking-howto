// Copyright Egor Ermolovich && Vladislav Alenik, 2024

// Feature test macro.
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>

#include <assert.h>

//----------------------------
// Параметры тестового стенда
//----------------------------

#define NUM_THREADS 8U
#define NUM_HARDWARE_THREAD 8U

const size_t NUM_ITERATIONS = 1000000U;

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct
{
    size_t thread_i;
    int* mutex;
} THREAD_ARGS;

// Переменная, доступ к которой должен быть защищён с помощью критической секции.
uint32_t var = 0U;

// Состояния мьютекса.
enum
{
    // Мьютекс разблокирован.
    M_ULOCKD = 0,
    // Мьютекс заблокирован.
    M_LOCKD = 1,
    // Мьютекс заблокирован и за время блокировки имел как минимум один поток в очереди ожидания.
    M_LOCKD_WQ = 2
};

// Обёртка системного вызова futex().
static int futex(
    int* uaddr,
    int futex_op,
    int val,
    const struct timespec *timeout,
    int* uaddr2,
    int val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

// Блокировка мьютекса.
void lock(int *mutex)
{
    // Ожидаемое предыдущее состояние фьютекса.
    int status = M_ULOCKD;

    // (1) Пытаемся атомарно преваритить разблокированный мьютекс в заблокированный.
    // В значение status записывается текущее состояние мьютекса.
    atomic_compare_exchange_strong_explicit(mutex, &status, M_LOCKD, memory_order_acquire, memory_order_relaxed);

    if (status != M_ULOCKD)
    { // До выполнения операции (1) мьютекс уже был заблокирован.
        if (status != M_LOCKD_WQ)
        {   // До выполнения операции (1) мьютекс был заблокирован единожды и не имел потоков в очереди ожидания.

            // (2) Атомарно записываем в мьютекс состояние
            // "заблокирован и за время блокировки имел как минимум один поток в очереди ожидания".
            // В значение status записывается текущее состояние мьютекса.
            status = atomic_exchange_explicit(mutex, M_LOCKD_WQ, memory_order_acquire);
        }

        // Ожидаем освобождения мьютекса.
        while (status != M_ULOCKD)
        {
            // Если состояние фьютекса M_LOCKD_WQ, то становимся в очередь ожидания для данного объекта фьютекса.
            // В планировщике состояние процесса меняется на "заблокирован".
            // Выход из данного системного вызова возможен по вызову в операции unlock.
            futex(mutex, FUTEX_WAIT, M_LOCKD_WQ, NULL, NULL, 0);

            // Атомарно производим блокировку.
            status = atomic_exchange_explicit(mutex, M_LOCKD_WQ, memory_order_acquire);
        }
    }
}

// Разблокировка мьютекса.
void unlock(int *mutex)
{
    // Атомарное разблокирование счётчика.
    if (atomic_fetch_sub_explicit(mutex, 1, memory_order_relaxed) != M_LOCKD)
    {
        // В случае успеха обновляем состояние фьютекса.
        atomic_store_explicit(mutex, M_ULOCKD, memory_order_release);

        // Оповещаем другие потоки, заблокированные на фьютексе.
        futex(mutex, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;
    printf("I am thread#%zu\n", args->thread_i);

    for (size_t i = 0U; i < NUM_ITERATIONS; ++i)
    {
        // Организуем критическую секцию для переменной var.
        lock(args->mutex);

        if (*args->mutex != M_LOCKD && *args->mutex != M_ULOCKD && *args->mutex != M_LOCKD_WQ) {
            printf("Futex implementation is invalid\n");
        }

        var++;

        unlock(args->mutex);
    }

    return NULL;
}

//--------------------------------
// Инициализация тестового стенда
//--------------------------------

typedef struct
{
    pthread_t tid;
} THREAD_INFO;

// Параметр для изучения явлений происходящих при расположении
// объекта синхронизации на границе кеш-линий.
// Почему при сдвиге 61 программа работает в 10 раз медленнее, чем при сдвиге 60?
#define MUTEX_ALIGNMENT_SHIFT 56U

int main()
{
    // Инициализируем объект синхронизации.
    _Alignas(64) char mutex[128] = {};
    int* mutex_var = (int*) &mutex[MUTEX_ALIGNMENT_SHIFT];
    *mutex_var = M_ULOCKD;

    // Инициализируем параметры потоков.
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].mutex    = mutex_var;
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
