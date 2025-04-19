// Сopyright Vladislav Aleinik, 2025
#include "server-common-multiplexing.h"

#include <memory.h>
#include <poll.h>

#include <sched.h>
#include <pthread.h>

//=============================================================
// Организация синхронного мультиплексирования при помощи poll
//=============================================================

void poll_wait_for_client(struct pollfd* pollfds, nfds_t* num_pollfds, FILESHARE_SERVER* server)
{
    struct pollfd* pollfd = &pollfds[*num_pollfds];

    pollfd->fd      = server->listen_sock_fd;
    pollfd->events  = POLLIN;
    pollfd->revents = 0U;

    *num_pollfds += 1U;
}

void poll_wait_for_data(struct pollfd* pollfds, nfds_t* num_pollfds, FILESHARE_SERVER* server)
{
    struct pollfd* pollfd = &pollfds[*num_pollfds];

    pollfd->fd      = server->src_file_fd;
    pollfd->events  = POLLIN;
    pollfd->revents = 0U;

    *num_pollfds += 1U;
}

void poll_wait_on_connection(struct pollfd* pollfds, nfds_t* num_pollfds, FILESHARE_CONNECTION* conn)
{
    struct pollfd* pollfd = &pollfds[*num_pollfds];

    pollfd->fd      = conn->client_sock_fd;
    pollfd->events  = POLLOUT|POLLHUP;
    pollfd->revents = 0U;

    *num_pollfds += 1U;
}

//============================
// Основная процедура сервера
//============================

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: server <src-file> <num-clients>\n");
        exit(EXIT_FAILURE);
    }

    char* endptr = argv[2];
    long max_conns = strtol(argv[2], &endptr, 10);
    if (*argv[2] == '\0' || *endptr != '\0' || max_conns <= 0)
    {
        fprintf(stderr, "Unable to parse number of clients!\n");
        exit(EXIT_FAILURE);
    }

    // Структура данных с представлением сервера.
    FILESHARE_SERVER server;

    // Аллоцируем буферы данных соединений.
    uint8_t* buffer = calloc(max_conns, TRANSFER_BLOCK_SIZE);
    if (buffer == NULL)
    {
        fprintf(stderr, "Unable to allocate connection buffers\n");
        exit(EXIT_FAILURE);
    }

    // Инициализируем соединия с клиентами.
    FILESHARE_CONNECTION* conns = calloc(max_conns, sizeof(FILESHARE_CONNECTION));
    if (conns == NULL)
    {
        fprintf(stderr, "Unable to allocate connection states\n");
        exit(EXIT_FAILURE);
    }

    for (long conn_i = 0U; conn_i < max_conns; conn_i++)
    {
        conns[conn_i].buffer = &buffer[TRANSFER_BLOCK_SIZE * conn_i];
        conns[conn_i].state  = CONNECTION_EMPTY;
    }

    // Аллоцируем массив файловых дескрипторов для мониторинга.
    struct pollfd* pollfds = calloc(max_conns + 1U, sizeof(struct pollfd));
    if (pollfds == NULL)
    {
        fprintf(stderr, "Unable to allocate poll file descriptor array\n");
        exit(EXIT_FAILURE);
    }

    // Открываем файл для раздачи.
    const char* src_filename = argv[1];
    server_open_src_file(&server, src_filename);

    // Настраиваем действие по нажатию Ctrl+C в консоли.
    // Код сервера не использует этот механизим.
    // Возмодное адекватное применение - окончание подключений новых клиентов.
    init_shutdown_control();

    // Активируем подключение клиентов.
    server_init_listen_socket(&server);

    // Количество подключенных клиентов.
    size_t num_active_clients = 0U;
    // Количество принятых запросов на подключение.
    size_t num_connected_clients = 0U;

    while (true)
    {
        // Условие выхода из цикла.
        bool exit_condition = false;

        // Запрет на обработку соединений от новых клиентов.
        bool accept_new_connections = num_connected_clients != max_conns && !program_in_shutdown();

        if (num_active_clients == 0U && !accept_new_connections)
        {
            // Выходим из цикла, если все текущие клиенты уже обработаны и если новых клиентов не будет.
            break;
        }

        // Количество дескрипторов для ожидания при вызове poll.
        nfds_t num_pollfds = 0U;

        if (accept_new_connections)
        {
            // Ожидаем подключения ещё одного клиента.
            poll_wait_for_client(pollfds, &num_pollfds, &server);
        }

        for (size_t conn_i = 0U; conn_i < num_connected_clients; ++conn_i)
        {
            switch (conns[conn_i].state)
            {
            case CONNECTION_INITIALIZED:
            case SEND_FILE_SIZE:
                poll_wait_on_connection(pollfds, &num_pollfds, &conns[conn_i]);
                break;
            case SEND_FILE_SIZE:
            case WRITE_DATA_TO_SOCKET:
                poll_wait_for_data(pollfds, &num_pollfds, &server);


            }
        }
    }

    // Останавливаем приём новых клиентов.
    server_close_listen_socket(&server);
    // Закрываем файл.
    server_close_src_file(&server);

    printf("Transfer finished\n");

    return EXIT_SUCCESS;
}
