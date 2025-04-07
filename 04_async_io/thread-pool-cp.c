// Copyright 2025, Vladislav Aleinik
#include "common.h"

#include <memory.h>

#include <sched.h>
#include <pthread.h>

//=================================
// Параметры процедуры копирования
//=================================

#define NUM_THREADS             2U
#define NUM_HARDWARE_THREADS    1U
#define READ_BLOCK_SIZE         4096U

//========================================
// Организация многопоточного копирования
//========================================

typedef struct {
    size_t thread_i;
    uint8_t* buffer;
    size_t src_size;
    int src_fd;
    int dst_fd;
} THREAD_ARGS;

typedef struct {
    pthread_t tid;
} THREAD_INFO;

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    //===================
    // Копирование файла
    //===================

    for (uint32_t i = 0U; i < args->src_size;)
    {
        // Вычисление сдвига в файле.
        size_t offset = i + args->thread_i * READ_BLOCK_SIZE;
        if (offset > args->src_size)
        {
            break;
        }

        // Чтение данных в буфер.
        ssize_t bytes_read = pread(args->src_fd, args->buffer, READ_BLOCK_SIZE, offset);
        if (bytes_read == -1)
        {
            fprintf(stderr, "Unable to read block [%x, %x)\n", i, i + READ_BLOCK_SIZE);
            exit(EXIT_FAILURE);
        }

        // Запись данных из буфера.
        ssize_t bytes_written = pwrite(args->dst_fd, args->buffer, bytes_read, offset);
        if (bytes_written == -1 || bytes_written != bytes_read)
        {
            fprintf(stderr, "Unable to write block [%x, %lx)\n", i, i + bytes_read);
            exit(EXIT_FAILURE);
        }

        i += READ_BLOCK_SIZE * NUM_THREADS;
        if (bytes_read != READ_BLOCK_SIZE)
        {
            break;
        }
    }

    return NULL;
}

//=======================
// Процедура копирования
//=======================

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: thread-pool-cp <src> <dst>");
        exit(EXIT_FAILURE);
    }

    // Открываем исходный файл и определяем его размер.
    int src_fd;
    uint32_t src_size;
    open_src_file(argv[1], &src_fd, &src_size);

    // Открываем результирующий файл и аллоцируем место на диске.
    int dst_fd;
    open_dst_file(argv[2], &dst_fd, src_size);

    // Выделяем память для промежуточного буфера.
    uint8_t* buffer = (uint8_t*) aligned_alloc(READ_BLOCK_SIZE, READ_BLOCK_SIZE * NUM_THREADS);
    if (buffer == NULL)
    {
        fprintf(stderr, "Unable to allocate aligned buffer\n");
        exit(EXIT_FAILURE);
    }

    //=======================
    // Создание пула потоков
    //=======================

    // Инициализируем данные потоков.
    THREAD_ARGS args[NUM_THREADS];
    for (size_t i = 0U; i < NUM_THREADS; ++i)
    {
        args[i].thread_i = i;
        args[i].buffer   = &buffer[i * READ_BLOCK_SIZE];
        args[i].src_size = src_size;
        args[i].src_fd   = src_fd;
        args[i].dst_fd   = dst_fd;
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

    // Закрываем файлы.
    close_src_dst_files(argv[1], src_fd, src_size, argv[2], dst_fd);

    return EXIT_SUCCESS;
}
