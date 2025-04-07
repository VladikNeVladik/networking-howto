// Copyright 2025, Vladislav Aleinik
#include "common.h"

#include <memory.h>
#include <libaio.h>

//=================================
// Параметры процедуры копирования
//=================================

#define READ_BLOCK_SIZE 4096U
#define QUEUE_SIZE 16U
#define FILE_SIZE "256M"

//==============
// Операции AIO
//==============

void io_read_setup(struct iocb* aio, int fd, off_t offset, void *buf, size_t size)
{
    // Заполяем информацию о текущем запросе.
    memset(aio, 0, sizeof(struct iocb));

    aio->aio_fildes     = fd;           // Файловый дескриптор файла чтения.
    aio->aio_lio_opcode = IO_CMD_PREAD; // Команда.
    aio->aio_reqprio    = 0;            // Приоритет запроса.
    aio->u.c.buf        = buf;          // Буфер данных, в который будем производить чтение.
    aio->u.c.nbytes     = size;         // Кол-во байт данных для считывания.
    aio->u.c.offset     = offset;       // Сдвиг от начала файла.
}

void io_write_setup(struct iocb* aio, int fd, off_t offset, void *buf, size_t size)
{
    // Заполяем информацию о текущем запросе.
    memset(aio, 0, sizeof(struct iocb));

    aio->aio_fildes     = fd;            // Файловый дескриптор файла для записи.
    aio->aio_lio_opcode = IO_CMD_PWRITE; // Команда.
    aio->aio_reqprio    = 0;             // Приоритет запроса.
    aio->u.c.buf        = buf;           // Буфер с данными, которые будем записывать в файл.
    aio->u.c.nbytes     = size;          // Кол-во байт данных для записи.
    aio->u.c.offset     = offset;        // Сдвиг от начала файла.
}

//=======================
// Процедура копирования
//=======================

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: linux-aio-cp <src> <dst>");
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

    //===================
    // Копирование файла
    //===================

    // Контекст AIO
    io_context_t io_ctx;
    memset(&io_ctx, 0, sizeof(io_ctx));

    int setup_ret = io_setup(QUEUE_SIZE, &io_ctx);
    if (setup_ret != 0)
    {
        fprintf(stderr, "Unable to setup AIO context\n");
        exit(EXIT_FAILURE);
    }

    // Буферы ввода-вывода.
    struct iocb iocbs[QUEUE_SIZE];

    // Массив обрабатываемых запросов.
    struct io_event events[QUEUE_SIZE];

    // Массив запросов для передачи в ОС.
    struct iocb* submit_list[QUEUE_SIZE];
    for (size_t aio_i = 0U; aio_i < QUEUE_SIZE; ++aio_i)
    {
        submit_list[aio_i] = NULL;
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
        io_read_setup(&iocbs[aio_i], src_fd, src_off,
            &buffer[aio_i * READ_BLOCK_SIZE], READ_BLOCK_SIZE);

        // Добавляем запрос в список запросов для передачи в ОС.
        submit_list[aio_i] = &iocbs[aio_i];

        src_off += READ_BLOCK_SIZE;
    }

    // Производим обработку до тех пор, пока есть выполняющиеся задачи.
    size_t num_to_submit = num_io_reqs;
    while (num_io_reqs != 0U)
    {
        // Передаём запросы в ОС.
        int submit_ret = io_submit(io_ctx, num_to_submit, submit_list);
        if (submit_ret < 0)
        {
            fprintf(stderr, "Unable to submit I/Os\n");
            exit(EXIT_FAILURE);
        }

        // Ожидаем выполнения хотя бы одной задачи.
        int num_events = io_getevents(io_ctx, 1U, QUEUE_SIZE, events, NULL);
        if (num_events < 0)
        {
            printf("Unable to get finished I/O events\n");
            exit(EXIT_FAILURE);
        }

        // Читаем полученный из ядра список окончившихся запросов.
        num_to_submit = 0U;
        for (int ev = 0U; ev < num_events; ++ev)
        {
            // Получаем управляющий блок запроса.
            struct iocb* iocb = events[ev].obj;
            int io_ret        = events[ev].res;

            if (iocb->aio_lio_opcode == IO_CMD_PREAD)
            {   // Выполнялась операция чтения.
                int bytes_read = io_ret;
                if (bytes_read != 0)
                {
                    // Подготавливаем запроса на запись.
                    io_write_setup(iocb, dst_fd, iocb->u.c.offset, iocb->u.c.buf, bytes_read);

                    // Добавляем запрос в список для передачи в ядро.
                    submit_list[num_to_submit] = iocb;
                    num_to_submit++;
                }
                else
                {
                    printf("ERROR: reach unavailible state\n");
                    exit(EXIT_FAILURE);
                }

            }
            else if (iocb->aio_lio_opcode == IO_CMD_PWRITE)
            {   // Выполнялась операция записи.
                int bytes_written = io_ret;
                if (bytes_written != 0 && src_off < src_size)
                {
                    // Подготавливаем запроса на следующее чтение.
                    io_read_setup(iocb, src_fd, src_off, iocb->u.c.buf, READ_BLOCK_SIZE);

                    // Добавляем запрос в список для передачи в ядро.
                    submit_list[num_to_submit] = iocb;
                    num_to_submit++;

                    src_off += READ_BLOCK_SIZE;
                }
                else
                {
                    num_io_reqs -= 1U;
                }
            }
        }
    }

    // Закрываем файлы.
    close_src_dst_files(argv[1], src_fd, src_size, argv[2], dst_fd);

    return EXIT_SUCCESS;
}
