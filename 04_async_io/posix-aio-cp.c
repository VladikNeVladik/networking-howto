// Copyright 2025, Vladislav Aleinik
#include "common.h"

#include <memory.h>
#include <aio.h>

//=================================
// Параметры процедуры копирования
//=================================

#define READ_BLOCK_SIZE 4096U
#define QUEUE_SIZE 16U

//==============
// Операции AIO
//==============

void aio_read_setup(struct aiocb* aio, int fd, off_t offset, volatile void *buf, size_t size)
{
    // Заполяем информацию о текущем запросе.
    memset(aio, 0, sizeof(struct aiocb));

    aio->aio_fildes = fd;       // Файловый дескриптор файла чтения.
    aio->aio_buf    = buf;      // Буфер данных, в который будем производить чтение.
    aio->aio_nbytes = size;     // Кол-во байт данных для считывания.
    aio->aio_offset = offset;   // Сдвиг от начала файла.

    // Инициируем операцию чтения.
    if (aio_read(aio) == -1)
    {
        perror("Unable to request read");
        exit(EXIT_FAILURE);
    }
}

void aio_write_setup(struct aiocb* aio, int fd, off_t offset, volatile void *buf, size_t size)
{
    // Заполяем информацию о текущем запросе.
    memset(aio, 0, sizeof(struct aiocb));

    aio->aio_fildes = fd;       // Файловый дескриптор файла для записи.
    aio->aio_buf    = buf;      // Буфер с данными, которые будем записывать в файл.
    aio->aio_nbytes = size;     // Кол-во байт данных для записи.
    aio->aio_offset = offset;   // Сдвиг от начала файла.

    // Инициируем операцию записи.
    if (aio_write(aio) == -1)
    {
        perror("Unable to request write");
        exit(EXIT_FAILURE);
    }
}

//=======================
// Процедура копирования
//=======================

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: posix-aio-cp <src> <dst>");
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
    uint8_t* buffer = (uint8_t*) aligned_alloc(READ_BLOCK_SIZE, READ_BLOCK_SIZE * QUEUE_SIZE);
    if (buffer == NULL)
    {
        fprintf(stderr, "Unable to allocate aligned buffer\n");
        exit(EXIT_FAILURE);
    }

    //====================================
    // Подготавливаем мета-информация AIO
    //====================================

    // Выделяем массив управляющих блоков AIO.
    struct aiocb* aiocbs = calloc(QUEUE_SIZE, sizeof(struct aiocb));
    if (aiocbs == NULL)
    {
        fprintf(stderr, "Unable to allocate AIO control blocks\n");
        exit(EXIT_FAILURE);
    }

    // Массив выполняющихся запросов AIO.
    struct aiocb* wait_list[QUEUE_SIZE];
    for (size_t aio_i = 0U; aio_i < QUEUE_SIZE; ++aio_i)
    {
        wait_list[aio_i] = NULL;
    }

    //===================
    // Копирование файла
    //===================

    // Округляем размер файла.
    src_size = src_size + READ_BLOCK_SIZE - (src_size % READ_BLOCK_SIZE);

    // Запускам первоначальный набор чтений.
    off_t src_off = 0U;
    size_t num_io_reqs = 0U;
    for (size_t aio_i = 0U; aio_i < QUEUE_SIZE && src_off < src_size; ++aio_i, ++num_io_reqs)
    {
        aio_read_setup(&aiocbs[aio_i], src_fd, src_off,
            &buffer[aio_i * READ_BLOCK_SIZE], READ_BLOCK_SIZE);

        // Размещаем AIO в список выполняющихся запросов.
        wait_list[aio_i] = &aiocbs[aio_i];

        src_off += READ_BLOCK_SIZE;
    }

    // Производим обработку до тех пор, пока есть выполняющиеся задачи.
    while (num_io_reqs != 0U)
    {
        // Ожидаем выполнения хотя бы одной задачи из списка.
        int suspend_ret = aio_suspend((const struct aiocb * const*) wait_list, QUEUE_SIZE, NULL);
        if (suspend_ret == -1)
        {
            printf("Unable to suspend-wait for AIOs\n");
            exit(EXIT_FAILURE);
        }

        // Проверяем, какие из задач закончили выполнение.
        for (size_t aio_i = 0U; aio_i < QUEUE_SIZE; ++aio_i)
        {
            // Пропускаем ячейку массива, если для неё нет активной задачи.
            if (wait_list[aio_i] == NULL) continue;

            // Пропускаем ячейку AIO, если запрос для неё ещё не завершился.
            int error_ret = aio_error(&aiocbs[aio_i]);
            if (error_ret == EINPROGRESS) continue;

            if (aiocbs[aio_i].aio_lio_opcode == LIO_READ)
            {   // Выполнялась операция чтения.
                // Получаем код возврата операции.
                int bytes_read = aio_return(&aiocbs[aio_i]);
                if (bytes_read != 0)
                {
                    // Запускаем операцию записи.
                    aio_write_setup(&aiocbs[aio_i], dst_fd, aiocbs[aio_i].aio_offset,
                        &buffer[aio_i * READ_BLOCK_SIZE], bytes_read);
                }
                else
                {
                    printf("ERROR: reach unavailible state\n");
                    exit(EXIT_FAILURE);
                }

            }
            else if (aiocbs[aio_i].aio_lio_opcode == LIO_WRITE)
            {   // Выполнялась операция записи.
                // Получаем код возврата операции.
                int bytes_written = aio_return(&aiocbs[aio_i]);
                if (bytes_written != 0 && src_off < src_size)
                {
                    // Инициируем следующую операцию записи.
                    aio_read_setup(&aiocbs[aio_i], src_fd, src_off,
                        &buffer[aio_i * READ_BLOCK_SIZE], READ_BLOCK_SIZE);

                    src_off += READ_BLOCK_SIZE;
                }
                else
                {
                    // Операция записи записала 0 байт (файл записан).
                    // Обозначаем ячейку AIO как неиспользуемую.
                    wait_list[aio_i] = NULL;

                    num_io_reqs -= 1U;
                }
            }
        }
    }

    // Закрываем файлы.
    close_src_dst_files(argv[1], src_fd, src_size, argv[2], dst_fd);

    return EXIT_SUCCESS;
}
