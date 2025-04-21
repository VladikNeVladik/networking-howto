// Сopyright Vladislav Aleinik, 2025
#include "server-common-multiplexing.h"

#include <memory.h>

#include <sched.h>
#include <pthread.h>

#include <sys/epoll.h>

//=============================================================
// Организация синхронного мультиплексирования при помощи epoll
//=============================================================

void epoll_server_wait_for_client(int epollfd, FILESHARE_SERVER* server)
{
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.u32 = 0U; // Уникальный идентификатор дескриптора.

    int ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, server->listen_sock_fd, &event);
    if (ret == -1)
    {
        fprintf(stderr, "[EPOLL WAIT FOR CLIENT] Unable to call epoll_ctl()\n");
        exit(EXIT_FAILURE);
    }
}

void epoll_server_stop_waiting_for_client(int epollfd, FILESHARE_SERVER* server)
{
    int ret = epoll_ctl(epollfd, EPOLL_CTL_DEL, server->listen_sock_fd, NULL);
    if (ret == -1)
    {
        fprintf(stderr, "[STOP WAITING FOR CLIENT] Unable to call epoll_ctl()\n");
        exit(EXIT_FAILURE);
    }
}

void epoll_conn_wait_on_socket(int epollfd, size_t conn_i, FILESHARE_CONNECTION* conn)
{
    struct epoll_event event;
    event.events = EPOLLOUT|EPOLLHUP;
    event.data.u32 = 1U + conn_i; // Уникальный идентификатор дескриптора.

    int ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, conn->client_sock_fd, &event);
    if (ret == -1)
    {
        fprintf(stderr, "[EPOLL WAIT ON SOCKET] Unable to call epoll_ctl()\n");
        exit(EXIT_FAILURE);
    }
}

void epoll_conn_stop_waiting_on_socket(int epollfd, FILESHARE_CONNECTION* conn)
{
    int ret = epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->client_sock_fd, NULL);
    if (ret == -1)
    {
        fprintf(stderr, "[STOP WAITING ON SOCKET] Unable to call epoll_ctl()\n");
        exit(EXIT_FAILURE);
    }
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

    // epoll-дескриптор для мультиплексирования запросов.
    int epollfd = epoll_create1(0U);
    if (epollfd == -1)
    {
        fprintf(stderr, "Unable to create epoll descriptor!\n");
        exit(EXIT_FAILURE);
    }

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

    // Аллоцируем массив событий для извлечения из epoll.
    struct epoll_event* events = calloc(max_conns + 1U, sizeof(struct epoll_event));
    if (events == NULL)
    {
        fprintf(stderr, "Unable to allocate epoll event array\n");
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

    // Инициируем ожидание на listen-сокете.
    epoll_server_wait_for_client(epollfd, &server);

    // Количество подключенных клиентов.
    size_t num_active_clients = 0U;
    // Количество принятых запросов на подключение.
    size_t num_connected_clients = 0U;

    bool accept_new_connections_prev = true;

    while (true)
    {
        // Запрет на обработку соединений от новых клиентов.
        bool accept_new_connections = num_connected_clients != max_conns && !program_in_shutdown();

        if (num_active_clients == 0U && !accept_new_connections)
        {
            // Выходим из цикла, если все текущие клиенты уже обработаны и если новых клиентов не будет.
            break;
        }

        if (accept_new_connections_prev && !accept_new_connections)
        {
            accept_new_connections_prev = false;

            // Останавливаем ожидание на listen-сокете.
            epoll_server_stop_waiting_for_client(epollfd, &server);
        }

        // Ожидаем
        int numevents = epoll_wait(epollfd, events, max_conns + 1U, /*infinite timeout*/ -1);
        if (numevents == -1)
        {
            fprintf(stderr, "Unable to epoll-wait for data on descriptors\n");
            exit(EXIT_FAILURE);
        }

        for (int event_i = 0; event_i < numevents; ++event_i)
        {
            struct epoll_event* ev = &events[event_i];

            if (ev->data.u32 == 0U)
            {   // Был получен запрос на подключение нового клиента.
                server_accept_connection_request(&server, &conns[num_connected_clients]);

                conns[num_connected_clients].state = SEND_FILE_SIZE;

                epoll_conn_wait_on_socket(epollfd, num_connected_clients, &conns[num_connected_clients]);

                num_connected_clients += 1U;
                num_active_clients += 1U;
                continue;
            }

            size_t conn_i = ev->data.u32 - 1U;

            if (ev->events & EPOLLHUP)
            {   // Соединение с клиентом оборвалось.
                epoll_conn_stop_waiting_on_socket(epollfd, &conns[conn_i]);
                server_close_conn_socket(&conns[conn_i]);
                conns[conn_i].state = TRANSFER_FINISHED;
                num_active_clients -= 1U;

                continue;
            }

            if (ev->events & EPOLLOUT)
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
                    epoll_conn_stop_waiting_on_socket(epollfd, &conns[conn_i]);
                    server_close_conn_socket(&conns[conn_i]);
                    conns[conn_i].state = TRANSFER_FINISHED;
                    num_active_clients -= 1U;
                }
            }

        }
    }

    // Останавливаем приём новых клиентов.
    server_close_listen_socket(&server);
    // Закрываем файл.
    server_close_src_file(&server);
    // Закрываем epoll-дескриптор.
    close(epollfd);

    printf("Transfer finished\n");

    return EXIT_SUCCESS;
}
