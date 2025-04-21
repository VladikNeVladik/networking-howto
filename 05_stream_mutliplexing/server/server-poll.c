// Сopyright Vladislav Aleinik, 2025
#include "server-common-multiplexing.h"

#include <memory.h>
#include <poll.h>

#include <sched.h>
#include <pthread.h>

//=============================================================
// Организация синхронного мультиплексирования при помощи poll
//=============================================================

void poll_server_wait_for_client(struct pollfd* pollfds, FILESHARE_SERVER* server)
{
    struct pollfd* pollfd = &pollfds[0U];

    pollfd->fd      = server->listen_sock_fd;
    pollfd->events  = POLLIN;
    pollfd->revents = 0U;
}

void poll_server_do_not_wait_for_client(struct pollfd* pollfds)
{
    struct pollfd* pollfd = &pollfds[0U];

    pollfd->fd      = -1;
    pollfd->events  = 0U;
    pollfd->revents = 0U;
}

void poll_conn_wait_on_socket(struct pollfd* pollfds, size_t conn_i, FILESHARE_CONNECTION* conn)
{
    struct pollfd* pollfd = &pollfds[1 + conn_i];

    pollfd->fd      = conn->client_sock_fd;
    pollfd->events  = POLLOUT|POLLHUP;
    pollfd->revents = 0U;
}

void poll_conn_do_not_wait_on_socket(struct pollfd* pollfds, size_t conn_i)
{
    struct pollfd* pollfd = &pollfds[1 + conn_i];

    pollfd->fd      = -1;
    pollfd->events  = 0U;
    pollfd->revents = 0U;
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
    size_t max_conns = (size_t) strtol(argv[2], &endptr, 10);
    if (*argv[2] == '\0' || *endptr != '\0' || max_conns <= 0)
    {
        fprintf(stderr, "Unable to parse number of clients!\n");
        exit(EXIT_FAILURE);
    }

    // Структура данных с представлением сервера.
    FILESHARE_SERVER server;

    // Инициализируем соединия с клиентами.
    FILESHARE_CONNECTION* conns = calloc(max_conns, sizeof(FILESHARE_CONNECTION));
    if (conns == NULL)
    {
        fprintf(stderr, "Unable to allocate connection states\n");
        exit(EXIT_FAILURE);
    }

    for (size_t conn_i = 0U; conn_i < max_conns; conn_i++)
    {
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
        // Запрет на обработку соединений от новых клиентов.
        bool accept_new_connections = num_connected_clients != max_conns && !program_in_shutdown();

        if (num_active_clients == 0U && !accept_new_connections)
        {
            // Выходим из цикла, если все текущие клиенты уже обработаны и если новых клиентов не будет.
            break;
        }

        if (accept_new_connections)
        {
            // Ожидаем подключения ещё одного клиента.
            poll_server_wait_for_client(pollfds, &server);
        }
        else
        {
            // Не ожидаем подключения ещё одного клиента.
            poll_server_do_not_wait_for_client(pollfds);
        }

        for (size_t conn_i = 0U; conn_i < num_connected_clients; ++conn_i)
        {
            switch (conns[conn_i].state)
            {
            case CONNECTION_EMPTY:
                fprintf(stderr, "Unexpected state!\n");
                exit(EXIT_FAILURE);
            case SEND_FILE_SIZE:
            case SEND_DATA_BLOCK:
                poll_conn_wait_on_socket(pollfds, conn_i, &conns[conn_i]);
                break;
            case TRANSFER_FINISHED:
                poll_conn_do_not_wait_on_socket(pollfds, conn_i);
                break;
            }
        }

        // Количество дескрипторов для передачи в poll.
        nfds_t nfds = 1U + num_connected_clients;

        int pollret = poll(pollfds, nfds, -1 /*infinite timeout*/);
        if (pollret == -1)
        {
            fprintf(stderr, "Unable to poll-wait for data on descriptors\n");
            exit(EXIT_FAILURE);
        }

        // Проверяем дескриптор для приёма новых клиентов.
        if (pollfds[0U].revents & POLLIN)
        {   // Был получен запрос на подключение нового клиента.
            server_accept_connection_request(&server, &conns[num_connected_clients]);

            conns[num_connected_clients].state = SEND_FILE_SIZE;

            num_connected_clients += 1U;
            num_active_clients += 1U;
        }

        for (size_t conn_i = 0U; conn_i < num_connected_clients; ++conn_i)
        {
            if (pollfds[1U + conn_i].revents & POLLHUP)
            {   // Соединение с клиентом оборвалось.
                server_close_conn_socket(&conns[conn_i]);
                conns[conn_i].state = TRANSFER_FINISHED;
                num_active_clients -= 1U;
                continue;
            }

            if (pollfds[1U + conn_i].revents & POLLOUT)
            {
                // Признак успеха операции.
                bool success = false;

                switch (conns[conn_i].state)
                {
                case CONNECTION_EMPTY:
                    fprintf(stderr, "Unexpected state!\n");
                    exit(EXIT_FAILURE);
                case SEND_FILE_SIZE:
                    success = server_send_file_size(&server, &conns[conn_i]);
                    break;
                case SEND_DATA_BLOCK:
                    success = server_send_file_block(&server, &conns[conn_i]);
                    break;
                case TRANSFER_FINISHED:
                    break;
                }

                // Обрабатываем ошибку в операции.
                if (!success)
                {
                    server_close_conn_socket(&conns[conn_i]);
                    num_active_clients -= 1U;
                }
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
