// Сopyright Vladislav Aleinik, 2025

#include "server-common.h"

#include <memory.h>

#include <sched.h>
#include <pthread.h>

//==========================
// Организация пула потоков
//==========================

// Количество аппаратных потоков.
#define NUM_HARDWARE_THREADS 4U

typedef struct {
    // Номер клиента.
    long client_i;
    // Признак того, что клиент уже запущен.
    bool clent_active;
    // Идентификатор потока в пуле потоков.
    pthread_t tid;
    // Состояние соединения.
    FILESHARE_CONNECTION conn;
    // Указатель на состояние сервера.
    FILESHARE_SERVER* server;

} THREAD_ARGS;

void* thread_func(void* thread_args)
{
    THREAD_ARGS* args = (THREAD_ARGS*) thread_args;

    FILESHARE_SERVER* server = args->server;
    FILESHARE_CONNECTION* conn = &args->conn;

    // Передаём размер файла клиенту.
    bool success = server_send_file_size(server, conn);
    if (!success)
    {
        server_close_conn_socket(conn);
        return NULL;
    }

    // Передаём блоки файла по сети.
    while (conn->src_file_offset < server->src_file_size)
    {
        success = server_send_file_block(server, conn);
        if (!success)
        {
            server_close_conn_socket(conn);
            return NULL;
        }
    }

    server_close_conn_socket(conn);

    printf("Finish client#%ld\n", args->client_i);

    return NULL;
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
    long num_clients = strtol(argv[2], &endptr, 10);
    if (*argv[2] == '\0' || *endptr != '\0')
    {
        fprintf(stderr, "Unable to parse number of clients!\n");
        exit(EXIT_FAILURE);
    }

    // Структура данных с представлением сервера.
    FILESHARE_SERVER server;

    // Инициализируем данные потоков.
    THREAD_ARGS* args = calloc(num_clients, sizeof(THREAD_ARGS));
    if (args == NULL)
    {
        fprintf(stderr, "Unable to allocate thread arguments\n");
        exit(EXIT_FAILURE);
    }

    for (long i = 0U; i < num_clients; ++i)
    {
        args[i].clent_active = false;
        args[i].server = &server;
        args[i].client_i = i;
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

    for (long client_i = 0; !program_in_shutdown() && client_i < num_clients; ++client_i)
    {
        // Ждём подключения одного клиента.
        bool success = server_accept_connection_request(&server, &args[client_i].conn);
        if (!success)
        {
            break;
        }

        args[client_i].clent_active = true;

        // Инициализируем аттрибуты потока.
        pthread_attr_t thread_attributes;
        int ret = pthread_attr_init(&thread_attributes);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to call pthread_attr_init\n");
            exit(EXIT_FAILURE);
        }

        // Назначаем аппаратные потоки для потоков POSIX.
        cpu_set_t assigned_harts;
        CPU_ZERO(&assigned_harts);

        // Предположения о системе:
        // - Система имеет NUM_HARDWARE_THREAD аппаратных потоков.
        //   Это число возможно извлекать из системы напрямую
        // - Все аппаратные потоки с 0 по NUM_HARDWARE_THREAD-1 активны.
        //   Это требование может нарушаться при выходе из строя какого-нибудь из ядер процессора.
        size_t hart_i = client_i % NUM_HARDWARE_THREADS;
        CPU_SET(hart_i, &assigned_harts);

        // Устанавливаем аффинность потока.
        ret = pthread_attr_setaffinity_np(&thread_attributes, sizeof(cpu_set_t), &assigned_harts);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to call pthread_attr_setaffinity_np\n");
            exit(EXIT_FAILURE);
        }

        // Создаём потоки POSIX.
        ret = pthread_create(&args[client_i].tid, &thread_attributes, thread_func, &args[client_i]);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to create thread\n");
            exit(EXIT_FAILURE);
        }

        // Удаляем объект с аттрибутами потока.
        pthread_attr_destroy(&thread_attributes);
    }

    // Завершаем текущую обработку клиентов.
    for (long i = 0; i < num_clients; ++i)
    {
        if (!args[i].clent_active)
        {
            continue;
        }

        int ret = pthread_join(args[i].tid, NULL);
        if (ret != 0)
        {
            fprintf(stderr, "Unable to join thread\n");
            exit(EXIT_FAILURE);
        }
    }

    // Останавливаем приём новых клиентов.
    server_close_listen_socket(&server);
    // Закрываем файл.
    server_close_src_file(&server);

    printf("Transfer finished\n");

    return EXIT_SUCCESS;
}
