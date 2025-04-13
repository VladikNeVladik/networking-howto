// Сopyright Vladislav Aleinik, 2025
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

//================
// Данные клиента
//================

typedef struct
{
    // Дескриптор сокета для подключения к серверу.
    int server_conn_fd;
    // Адрес для подключению к серверу.
    struct sockaddr server_addr;

    // Файловый дескриптор файла для чтения с сервера.
    int dst_file_fd;
    // Размер файла для чтения с сервера.
    size_t dst_file_size;

    // Сдвиг в файле для текущего копирования.
    size_t dst_file_offset;
} FILESHARE_CLIENT;

//==================
// Управление сетью
//==================

bool client_connect_to_server(FILESHARE_CLIENT* client)
{
    client->server_conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->server_conn_fd == -1)
    {
        fprintf(stderr, "[client_create_socket] Unable to create socket()\n");
        exit(EXIT_FAILURE);
    }

    // Формируем желаемый адрес для подключения.
    struct addrinfo hints;

    struct addrinfo* res;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = 0;
    hints.ai_protocol = 0;

    if (getaddrinfo("127.0.0.1", "1337", &hints, &res) != 0)
    {
        fprintf(stderr, "[client_create_socket] Unable to call getaddrinfo()\n");
        exit(EXIT_FAILURE);
    }

    // Проверяем, что для данного запроса существует только один адрес.
    if (res->ai_next != NULL)
    {
        fprintf(stderr, "[client_create_socket] Ambigous result of getaddrinfo()\n");
        exit(EXIT_FAILURE);
    }

    client->server_addr = *res->ai_addr;

    if (connect(client->server_conn_fd, &client->server_addr, sizeof(client->server_addr)) == -1)
    {
        if (errno == ECONNREFUSED)
        {
            close(client->server_conn_fd);

            return false;
        }

        fprintf(stderr, "[connect_to_master] Unable to connect() to master");
        exit(EXIT_FAILURE);
    }

    return true;
}

void client_close_socket(FILESHARE_CLIENT* client)
{
    if (close(client->server_conn_fd) == -1)
    {
        fprintf(stderr, "[client_close_socket] Unable to close() client socket\n");
        exit(EXIT_FAILURE);
    }
}

//=================
// Работа с файлом
//=================

void client_open_dst_file(FILESHARE_CLIENT* client, const char* filename)
{
    // Открываем файл на запись.
    client->dst_file_fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (client->dst_file_fd == -1)
    {
        fprintf(stderr, "Unable to open destination file '%s': errno=%i (%s)",
            filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Просим ОС превентивно выделить память под файл.
    // Это позволяет избавиться от итеративного довыделения памяти в процессе копирования.
    if (fallocate(client->dst_file_fd, 0, 0, client->dst_file_size) == -1)
    {
        fprintf(stderr, "Not enough space for file '%s': errno=%i (%s)",
            filename, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void client_close_dst_file(FILESHARE_CLIENT* client)
{
    // Отрезаем файл до необходимого размера.
    // Это необходимо, т.к. размер файла не кратен размеру блока записи.
    if (ftruncate(client->dst_file_fd, client->dst_file_size) == -1)
    {
        fprintf(stderr, "Unable to truncate file: errno=%i (%s)",
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Убеждаемся, что файл записан на диск.
    if (fsync(client->dst_file_fd) == -1)
    {
        fprintf(stderr, "Unable to sync file: errno=%i (%s)",
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (close(client->dst_file_fd) == -1)
    {
        fprintf(stderr, "Unable to close file: errno=%i (%s)",
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

//=================================
// Обработка соединения с сервером
//=================================

#define TRANSFER_BLOCK_SIZE 1024U

bool client_recv_file_size(FILESHARE_CLIENT* client)
{
    uint64_t file_size = 0U;

    size_t bytes_read = recv(client->server_conn_fd, &file_size, sizeof(file_size), MSG_WAITALL);
    if (bytes_read != sizeof(file_size))
    {
        fprintf(stderr, "Unable to recv file size from server\n");
        return false;
    }

    client->dst_file_size = htobe64(file_size);

    // Будем записывать файл с нулевой позиции.
    client->dst_file_offset = 0U;

    return true;
}

bool client_recv_file_block(FILESHARE_CLIENT* client)
{
    char file_block[TRANSFER_BLOCK_SIZE];

    size_t bytes_read = recv(client->server_conn_fd, file_block, TRANSFER_BLOCK_SIZE, 0U);
    if (bytes_read == 0 && client->dst_file_offset != client->dst_file_size)
    {
        fprintf(stderr, "Unable to recv data block from server\n");
        return false;
    }

    size_t bytes_written = pwrite(client->dst_file_fd, file_block, bytes_read, client->dst_file_offset);
    if (bytes_written != bytes_read)
    {
        fprintf(stderr, "Unable to write data block to file\n");
        exit(EXIT_FAILURE);
    }

    // Обновляем текущий сдвиг в файле.
    client->dst_file_offset += bytes_written;

    return true;
}

//============================
// Основная процедура клиента
//============================

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: client <dst-file>");
        exit(EXIT_FAILURE);
    }

    // Данные клиента.
    FILESHARE_CLIENT client;

    // Метка для повторной попытки подключения.
    start_connection:

    bool connected_to_server = client_connect_to_server(&client);
    while (!connected_to_server)
    {
        // Ожидаем, пока сервер проснётся.
        sleep(1U);

        printf("Wait for server to start\n");

        connected_to_server = client_connect_to_server(&client);
    }

    // Считываем размер файла.
    bool success = client_recv_file_size(&client);
    if (!success)
    {
        client_close_socket(&client);
        goto start_connection;
    }

    // Открываем файл для записи.
    const char* dst_filename = argv[1];
    client_open_dst_file(&client, dst_filename);

    // Считываем файл.
    while (client.dst_file_offset != client.dst_file_size)
    {
        success = client_recv_file_block(&client);
        if (!success)
        {
            client_close_socket(&client);
            goto start_connection;
        }
    }

    // Освобождаем сокет.
    client_close_socket(&client);
    // Синхронизируем состояние файла на диске.
    client_close_dst_file(&client);

    printf("Received file\n");

    return EXIT_SUCCESS;
}
