// Сopyright Vladislav Aleinik, 2025

#include "server-common.h"

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
    long num_clients = strtol(argv[2], &endptr, 10);
    if (*argv[2] == '\0' || *endptr != '\0')
    {
        fprintf(stderr, "Unable to parse number of clients!\n");
        exit(EXIT_FAILURE);
    }

    // Структура данных с представлением сервера.
    FILESHARE_SERVER server;

    // Открываем файл для раздачи.
    const char* src_filename = argv[1];
    server_open_src_file(&server, src_filename);

    // Настраиваем действие по нажатию Ctrl+C в консоли.
    // Код сервера не использует этот механизим.
    // Возмодное адекватное применение - окончание подключений новых клиентов.
    init_shutdown_control();

    // Активируем подключение клиентов.
    server_init_listen_socket(&server);

    // Создаём соединие с клиентом.
    FILESHARE_CONNECTION conn;

    for (long client_i = 0; !program_in_shutdown() && client_i < num_clients; ++client_i)
    {
        // Ждём подключения одного клиента.
        bool success = server_accept_connection_request(&server, &conn);
        if (!success)
        {
            break;
        }

        // Передаём размер файла клиенту.
        success = server_send_file_size(&server, &conn);
        if (!success)
        {
            server_close_conn_socket(&conn);
            continue;
        }

        // Передаём блоки файла по сети.
        while (conn.src_file_offset < server.src_file_size)
        {
            success = server_send_file_block(&server, &conn);
            if (!success)
            {
                server_close_conn_socket(&conn);
                continue;
            }
        }

        server_close_conn_socket(&conn);
    }

    // Останавливаем приём новых клиентов.
    server_close_listen_socket(&server);
    // Закрываем файл.
    server_close_src_file(&server);

    printf("Transfer finished\n");

    return EXIT_SUCCESS;
}
