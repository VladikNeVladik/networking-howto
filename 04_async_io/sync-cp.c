// Copyright 2025, Vladislav Aleinik
#include "common.h"

#include <memory.h>

//=================================
// Параметры процедуры копирования
//=================================

#define READ_BLOCK_SIZE 4096U

//=======================
// Процедура копирования
//=======================

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: sync-cp <src> <dst>");
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
    uint8_t* buffer = (uint8_t*) aligned_alloc(READ_BLOCK_SIZE, READ_BLOCK_SIZE);
    if (buffer == NULL)
    {
        fprintf(stderr, "Unable to allocate aligned buffer\n");
        exit(EXIT_FAILURE);
    }

    //===================
    // Копирование файла
    //===================

    for (uint32_t i = 0U; i < src_size;)
    {
        // Производим чтение в буфер.
        ssize_t bytes_read = read(src_fd, buffer, READ_BLOCK_SIZE);
        if (bytes_read == -1)
        {
            fprintf(stderr, "Unable to read block [%x, %x)\n", i, i + READ_BLOCK_SIZE);
            exit(EXIT_FAILURE);
        }

        // Производим запись из буфера.
        ssize_t bytes_written = write(dst_fd, buffer, bytes_read);
        if (bytes_written == -1 || bytes_written != bytes_read)
        {
            fprintf(stderr, "Unable to write block [%x, %lx)\n", i, i + bytes_read);
            exit(EXIT_FAILURE);
        }

        i += bytes_read;
        if (bytes_read != READ_BLOCK_SIZE)
        {
            break;
        }
    }

    // Закрываем файлы.
    close_src_dst_files(argv[1], src_fd, src_size, argv[2], dst_fd);

    return EXIT_SUCCESS;
}
