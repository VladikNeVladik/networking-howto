// Copyright 2025, Vladislav Aleinik
#ifndef MSUSEM_ASYNC_IO
#define MSUSEM_ASYNC_IO

#define _GNU_SOURCE

#include <stdint.h>
#include <stdbool.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

//======================
// Операции над файлами
//======================

void open_src_file(const char* filename, int* fd, uint32_t* file_size)
{
    // Открываем файл на чтение.
    // Флаг O_DIRECT обозначает чтение запись непосредственно из буферов user-space.
    *fd = open(filename, O_RDONLY|O_DIRECT);
    if (*fd == -1)
    {
        fprintf(stderr, "Unable to open source file '%s': errno=%i (%s)",
            filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Запрашиваем у ФС размер файла.
    struct stat statbuf;
    if (fstat(*fd, &statbuf) == -1)
    {
        fprintf(stderr, "Unable to determine source file size: errno=%i (%s)",
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    *file_size = statbuf.st_size;
}

void open_dst_file(const char* filename, int* fd, uint32_t src_size)
{
    // Открываем файл на запись.
    *fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (*fd == -1)
    {
        fprintf(stderr, "Unable to open destination file '%s': errno=%i (%s)",
            filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Просим ОС превентивно выделить память под файл.
    // Это позволяет избавиться итеративного довыделения памяти в процессе копирования.
    if (fallocate(*fd, 0, 0, src_size) == -1)
    {
        fprintf(stderr, "Not enough space for file '%s': errno=%i (%s)",
            filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void close_src_dst_files(
    const char* src_filename, int src_fd, uint32_t src_size,
    const char* dst_filename, int dst_fd)
{
    // Отрезаем файл до необходимого размера.
    // Это необходимо, т.к. размер файла не кратен размеру блока записи.
    if (ftruncate(dst_fd, src_size) == -1)
    {
        fprintf(stderr, "Unable to truncate file '%s': errno=%i (%s)",
            dst_filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Убеждаемся, что файл записан на диск.
    if (fsync(dst_fd) == -1)
    {
        fprintf(stderr, "Unable to sync file '%s': errno=%i (%s)",
            dst_filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Закрываем файлы.
    if (close(src_fd) == -1)
    {
        fprintf(stderr, "Unable to close file '%s': errno=%i (%s)",
            src_filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (close(dst_fd) == -1)
    {
        fprintf(stderr, "Unable to close file '%s': errno=%i (%s)",
            dst_filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

#endif // MSUSEM_ASYNC_IO
