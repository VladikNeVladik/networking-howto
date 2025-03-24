// No copyright. Vladislav Alenik && Evgeny Baskov, 2024

// Feature test macro:
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>

//============================
// Параметры тестового стенда
//============================

#define QUEUE_SIZE     64U
#define NUM_ITERATIONS 100000000ULL

#define ENABLE_PADDING 0

#define ENABLE_SIMPLE  1

#define ENABLE_BACKOFF 1
#define NUM_RETRIES    10U

#define NUM_HARDWARE_THREADS 2U

//---------------------------
// Lock-free кольцевой буфер
//---------------------------

#define CACHE_LINE_SIZE 256

typedef struct {
    uint64_t* data;
    uint32_t size;

    uint32_t cached_head;
#if ENABLE_PADDING == 1
    uint8_t pad0[CACHE_LINE_SIZE];
#endif
    uint32_t cached_tail;
#if ENABLE_PADDING == 1
    uint8_t pad1[CACHE_LINE_SIZE];
#endif
    _Atomic uint32_t head;
#if ENABLE_PADDING == 1
    uint8_t pad2[CACHE_LINE_SIZE];
#endif
    _Atomic uint32_t tail;
} QUEUE;

void queue_init(QUEUE* queue, uint32_t size)
{
    if (size == 0 || ((size - 1) & size) != 0)
    {
        printf("queue_init: size (%u) is expected to be power of two\n", size);
        exit(EXIT_FAILURE);
    }

    queue->data = (uint64_t*) calloc(size, sizeof(uint64_t));
    if (queue->data == NULL)
    {
        printf("queue_init: size (%u) is too big\n", size);
        exit(EXIT_FAILURE);
    }

    queue->size = size;
    queue->cached_head = 0U;
    queue->cached_tail = 0U;
    queue->head = 0U;
    queue->tail = 0U;
}

bool queue_enqueue(QUEUE* queue, uint64_t elem)
{
    uint32_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);

    if (tail - queue->cached_head == queue->size)
    {
        uint32_t head = atomic_load_explicit(&queue->head, memory_order_acquire);

        queue->cached_head = head;

        if (tail - head == queue->size)
        {
            return false;
        }
    }

    queue->data[tail & (queue->size - 1)] = elem;
    atomic_store_explicit(&queue->tail, tail + 1, memory_order_release);

    return true;
}

bool queue_dequeue(QUEUE* queue, uint64_t* elem)
{
    uint32_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);

    if (queue->cached_tail == head)
    {
        uint32_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
        queue->cached_tail = tail;

        if (tail == head)
        {
            return false;
        }
    }

    *elem = queue->data[head & (queue->size - 1)];
    atomic_store_explicit(&queue->head, head + 1, memory_order_release);

    return true;
}

bool queue_enqueue_simple(QUEUE* queue, uint64_t elem)
{
    // [1] Извлекаем голову кольцевой очереди (синхронизация с [4]).
    uint32_t head = atomic_load_explicit(&queue->head, memory_order_acquire);

    // Извлекаем хвост кольцевой очереди.
    uint32_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);

    if (tail - head == queue->size)
    {   // Очередь полна.
        // Добавление элемента в полную очередь невозможно.
        return false;
    }

    // Добавляем элемент в очередь.
    queue->data[tail & (queue->size - 1U)] = elem;

    // [2] Уведомляем читателя о появлении нового элемента в очереди (см. [3]).
    atomic_store_explicit(&queue->tail, tail + 1, memory_order_release);

    // Добавление элемента в очередь прошло успешно.
    return true;
}

bool queue_dequeue_simple(QUEUE* queue, uint64_t* elem)
{
    // Считываем голову кольцевой очереди
    uint32_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);

    // [3] Считываем хвост кольцевой очереди (см. [4]).
    uint32_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    if (tail == head)
    {   // Кольцевая очередь пуста.
        return false;
    }

    // Вычитываем элемент из очереди.
    *elem = queue->data[head & (queue->size - 1U)];

    // [4] Уведомляем писателя о появлении нового свободного места в очереди (см. [1]).
    atomic_store_explicit(&queue->head, head + 1, memory_order_release);

    // Извлечение элемента из очереди прошло успешно.
    return true;
}

//------------------------------
// Применение кольцевой очереди
//------------------------------

void thread_producer(QUEUE* queue)
{
    for (uint64_t snd_i = 0U; snd_i < NUM_ITERATIONS; ++snd_i)
    {
        bool success = false;
        uint32_t retry = 0U;
        do
        {
#if ENABLE_SIMPLE == 1
            success = queue_enqueue_simple(queue, snd_i);
#else
            success = queue_enqueue(queue, snd_i);
#endif
            retry++;

            if (ENABLE_BACKOFF && retry == NUM_RETRIES)
            {
                retry = 0U;
                pthread_yield();
            }
        }
        while (!success);
    }
}

void thread_consumer(QUEUE* queue)
{
    for (uint64_t rcv_i = 0U; rcv_i < NUM_ITERATIONS; ++rcv_i)
    {
        uint64_t snd_i = 0U;

        bool success = false;
        uint32_t retry = 0U;
        do
        {
#if ENABLE_SIMPLE == 1
            success = queue_dequeue_simple(queue, &snd_i);
#else
            success = queue_dequeue(queue, &snd_i);
#endif
            retry++;

            if (ENABLE_BACKOFF && retry == NUM_RETRIES)
            {
                retry = 0U;
                pthread_yield();
            }
        }
        while (!success);

        // Compare result:
        if (snd_i != rcv_i)
        {
            printf("Invalid queue element: expected %lu, got %lu\n", rcv_i, snd_i);
            exit(EXIT_FAILURE);
        }
    }
}

//-------------------------------
// Совместное исполнение потоков
//-------------------------------

typedef struct {
    size_t thread_i;
    QUEUE* queue;
} THREAD_ARGS;

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    if (args->thread_i == 0U)
    {
        thread_producer(args->queue);
    }
    else
    {
        thread_consumer(args->queue);
    }

    return NULL;
}

//--------------------------------
// Инициализация тестового стенда
//--------------------------------

#define NUM_THREADS 2U

typedef struct {
    pthread_t tid;
} THREAD_INFO;

int main()
{
    QUEUE queue;
    queue_init(&queue, QUEUE_SIZE);

    // Инициализируем параметры потоков.
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].queue = &queue;
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
        // - Система имеет NUM_HARDWARE_THREADS аппаратных потоков.
        //   Это число возможно извлекать из системы напрямую
        // - Все аппаратные потоки с 0 по NUM_HARDWARE_THREADS-1 активны.
        //   Это требование может нарушаться при выходе из строя какого-нибудь из ядер процессора.
        size_t hart_i = i % NUM_HARDWARE_THREADS;
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

    return EXIT_SUCCESS;
}
