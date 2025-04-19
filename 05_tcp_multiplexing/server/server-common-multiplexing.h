// Сopyright Vladislav Aleinik, 2025
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <endian.h>
#include <arpa/inet.h>

//================
// Данные сервера
//================

typedef struct
{
    // Файловый дескриптор файла для распространения клиентам.
    int src_file_fd;
    // Размер файла для распространения клиентам.
    size_t src_file_size;

    // Дескриптор слушающего сокета для первоначального подключения клиентов.
    int listen_sock_fd;
} FILESHARE_SERVER;

//=============================
// Обработка одного соединения
//=============================

#define TRANSFER_BLOCK_SIZE 1024U

// Состояние передачи отдельного клиента.
typedef enum
{
    CONNECTION_EMPTY,       // (1) Соединение с данным клиентом не установлено.
    CONNECTION_INITIALIZED, // (2) Соединение с клиентом установлено, данные не передавались.
    SEND_FILE_SIZE,         // (3) Сервер передал клиенту размер файла.
    READ_DATA_BLOCK,        // (4) Сервер считал с диска очередной блок данных.
    WRITE_DATA_TO_SOCKET,   // (5) Сервер передал клиенту очередной блок данных.
    TRANSFER_FINISHED       // (6) Сервер передал клиенту все блоки данных.
} TRANSFER_STATE;

// Условия переходов между состояниями:
// (1) -> (2) - Слушающий сокет получил очередной запрос на подключение от клиента.
// (2) -> (3) - Была возможна запись в сетевое соединие.
// (3) -> (4) - Было возможно чтение из файла.
// (4) -> (5) - Была возможна запись в сетевое соединие.
// (5) -> (4) - клиенту передан ещё не весь файл, было возможно чтение из файла.
// (5) -> (6) - клиенту передан весь файл.

typedef struct
{
    // Дескриптор сокета для обмена данными с клиентом.
    int client_sock_fd;
    // Сдвиг в файле для текущего копирования.
    size_t src_file_offset;

    // Указатель на массив данных клиента.
    uint8_t* buffer;
    // Текущее состояние протокола обмена данными с данным клиентом.
    TRANSFER_STATE state;

} FILESHARE_CONNECTION;

bool server_send_file_size(const FILESHARE_SERVER* server, FILESHARE_CONNECTION* conn)
{
    uint64_t file_size = htobe64(server->src_file_size);

    size_t bytes_written = write(conn->client_sock_fd, &file_size, sizeof(file_size));
    if (bytes_written != sizeof(file_size))
    {
        fprintf(stderr, "Unable to send file size to client\n");
        return false;
    }

    // Будем копировать файл с нулевой позиции.
    conn->src_file_offset = 0U;

    return true;
}

bool server_read_file_block(const FILESHARE_SERVER* server, FILESHARE_CONNECTION* conn)
{
    size_t bytes_read = pread(server->src_file_fd, file_block, TRANSFER_BLOCK_SIZE, conn->src_file_offset);
    if (bytes_read == 0 && conn->src_file_offset != server->src_file_size)
    {
        fprintf(stderr, "Unable to read data from file\n");
        return false;
    }

    return true;
}

bool server_send_file_block(const FILESHARE_SERVER* server, FILESHARE_CONNECTION* conn)
{
    size_t bytes_written = write(conn->client_sock_fd, file_block, bytes_read);
    if (bytes_written != bytes_read)
    {
        fprintf(stderr, "Unable to send data block to client\n");
        return false;
    }

    // Обновляем текущий сдвиг в файле.
    conn->src_file_offset += bytes_written;

    return true;
}

//=================
// Работа с файлом
//=================

void server_open_src_file(FILESHARE_SERVER* server, const char* filename)
{
    // Открываем файл на чтение.
    server->src_file_fd = open(filename, O_RDONLY | O_NONBLOCK);
    if (server->src_file_fd == -1)
    {
        fprintf(stderr, "Unable to open source file '%s': errno=%i (%s)\n",
            filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Запрашиваем у ФС размер файла.
    struct stat statbuf;
    if (fstat(server->src_file_fd, &statbuf) == -1)
    {
        fprintf(stderr, "Unable to determine source file size: errno=%i (%s)\n",
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    server->src_file_size = statbuf.st_size;
}

void server_close_src_file(FILESHARE_SERVER* server)
{
    // Закрываем файл.
    if (close(server->src_file_fd) == -1)
    {
        fprintf(stderr, "Unable to close file: errno=%i (%s)\n",
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

//===========================
// Завершение работы сервера
//===========================

static _Atomic bool received_sigint = false;

bool program_in_shutdown()
{
    return atomic_load(&received_sigint);
}

void sigint_handler(int)
{
    atomic_store(&received_sigint, true);
}

void init_shutdown_control()
{
    // Маскируем все остальные сигналы на время вызова обработчика сигнала.
    sigset_t block_all_signals;
    if (sigfillset(&block_all_signals) == -1)
    {
        fprintf(stderr, "[init_shutdown_control] Unable to set signal mask\n");
        exit(EXIT_FAILURE);
    }

    // Выставляем обработчик сигнала SIGINT.
    struct sigaction act =
    {
        .sa_handler = sigint_handler,
        .sa_mask    = block_all_signals,
        .sa_flags   = 0
    };
    if (sigaction(SIGINT, &act, NULL) == -1)
    {
        fprintf(stderr, "[init_shutdown_control] Unable to set SIGINT handler\n");
        exit(EXIT_FAILURE);
    }
}

//==================
// Управление сетью
//==================

void server_init_listen_socket(FILESHARE_SERVER* server)
{
    // Создаём сокет, слушающий подключения клиентов.
    server->listen_sock_fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0);
    if (server->listen_sock_fd == -1)
    {
        fprintf(stderr, "[server_init_listen_socket] Unable to create socket!\n");
        exit(EXIT_FAILURE);
    }

    // Запрещаем перевод слушающего сокета в состояние TIME_WAIT.
    int setsockopt_yes = 1;
    if (setsockopt(server->listen_sock_fd, SOL_SOCKET, SO_REUSEADDR, &setsockopt_yes, sizeof(setsockopt_yes)) == -1)
    {
        fprintf(stderr, "[server_init_listen_socket] Unable to set SO_REUSEADDR socket option\n");
        exit(EXIT_FAILURE);
    }

    // Формируем адрес для прослушивания запросов на подключение.
    struct sockaddr_in listen_addr;
    listen_addr.sin_family      = AF_INET;           // Семейство запращиваемого адреса: IPv4-адрес.
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Размещаем устройство по произвольному адресу.
    listen_addr.sin_port        = htons(1337);       // Номер порта для подключения.

    if (bind(server->listen_sock_fd, (struct sockaddr*) &listen_addr, sizeof(listen_addr)) == -1)
    {
        fprintf(stderr, "[server_init_listen_socket] Unable to bind\n");
        exit(EXIT_FAILURE);
    }

    // Активируем очередь запросов на подключение.
    if (listen(server->listen_sock_fd, 10U /* Размер очереди запросов на подключение */) == -1)
    {
        fprintf(stderr, "[server_init_listen_socket] Unable to listen() on a socket\n");
        exit(EXIT_FAILURE);
    }
}

void server_close_listen_socket(FILESHARE_SERVER* server)
{
    if (close(server->listen_sock_fd) == -1)
    {
        fprintf(stderr, "[server_close_listen_socket] Unable to close() listen-socket\n");
        exit(EXIT_FAILURE);
    }
}

bool server_accept_connection_request(FILESHARE_SERVER* server, FILESHARE_CONNECTION* conn)
{
    printf("Wait for client to connect\n");

    // Создаём сокет для клиента из очереди на подключение.
    conn->client_sock_fd = accept(server->listen_sock_fd, NULL, NULL);
    if (conn->client_sock_fd == -1)
    {
        if (program_in_shutdown())
        {
            return false;
        }

        fprintf(stderr, "[server_accept_connection_request] Unable to accept() connection on a socket\n");
        exit(EXIT_FAILURE);
    }

    // Разрешаем "зависания сокета" для доотправки данных.
    struct linger linger_params =
    {
        .l_onoff  = 1,
        .l_linger = 1
    };
    if (setsockopt(server->listen_sock_fd, SOL_SOCKET, SO_LINGER, &linger_params, sizeof(linger_params)) == -1)
    {
        fprintf(stderr, "[server_init_listen_socket] Unable to disable SO_LINGER socket option");
        exit(EXIT_FAILURE);
    }

    // Disable Nagle's algorithm:
    int setsockopt_arg = 1;
    if (setsockopt(conn->client_sock_fd, IPPROTO_TCP, TCP_NODELAY, &setsockopt_arg, sizeof(setsockopt_arg)) == -1)
    {
        fprintf(stderr, "[server_accept_connection_request] Unable to enable TCP_NODELAY socket option");
        exit(EXIT_FAILURE);
    }

    // Disable corking:
    setsockopt_arg = 0;
    if (setsockopt(conn->client_sock_fd, IPPROTO_TCP, TCP_CORK, &setsockopt_arg, sizeof(setsockopt_arg)) == -1)
    {
        fprintf(stderr, "[server_accept_connection_request] Unable to disable TCP_CORK socket option");
        exit(EXIT_FAILURE);
    }

    // Самостоятельное задание: как включить механизм TCP keepalive для детектирования обрыва соединения?

    printf("Client connected\n");

    return true;
}

void server_close_conn_socket(FILESHARE_CONNECTION* conn)
{
    if (close(conn->client_sock_fd) == -1)
    {
        fprintf(stderr, "[server_close_conn_socket] Unable to close() connection socket\n");
        exit(EXIT_FAILURE);
    }
}
