#define _POSIX_C_SOURCE 200809L // посикс-функции нужной версии

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h" // общий файл с константами

#define PROGRAM_NAME "client"

// функция вывода версии
static void print_version(void) {
    printf("%s\n", PROGRAM_NAME);
    printf("Автор: Спицына Полина Николаевна\n");
    printf("Группа: N3246\n");
    printf("Вариант: 23\n");
    printf("Транспорт: TCP\n");
}

// функция вывода справки
static void print_help(void) {
    printf("Использование: %s [опции] выражение\n", PROGRAM_NAME);
    printf("Опции:\n");
    printf("  -a ip      IPv4-адрес сервера\n");
    printf("  -p port    порт сервера\n");
    printf("  -h         справка\n");
    printf("  -v         версия\n");
    printf("Пример: %s -a 127.0.0.1 -p 5555 \"2 + 3 * 4\"\n", PROGRAM_NAME);
}

// функция, к-я собирает выражение из аргументов командной строки в одну строку
static char *join_args(int argc, char *argv[], int start) {
    size_t len = 0; // длина будущей строки
    char *res; // указатель на строку
    size_t pos = 0; // Текущая позиция, куда записываем символы в res

    for (int i = start; i < argc; i++) { // цикл по аргументам
        len += strlen(argv[i]) + 1;
    }

    res = malloc(len + 2);
    if (res == NULL) {
        return NULL;
    }
    // проходим второй раз по аргументам, чтобы их скопировать
    for (int i = start; i < argc; i++) {
        size_t cur = strlen(argv[i]);
        memcpy(res + pos, argv[i], cur);
        pos += cur;
        if (i + 1 < argc) {
            res[pos++] = ' ';
        }
    }
    res[pos++] = '\n';
    res[pos] = '\0';
    return res;
}

// функция, которая отправляет всю строку в сокет
static int write_all(int fd, const char *s) {
    size_t len = strlen(s);
    size_t sent = 0; // сколько байтов уже отправлено
    while (sent < len) {
        ssize_t n = write(fd, s + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    // берем айпи-адрес сервера (если переменная окружения задана, то используем ее)
    const char *addr = getenv("lab3ADDR") ? getenv("lab3ADDR") : DEFAULT_ADDR;
    // аналогично берем порт
    const char *port_str = getenv("lab3PORT") ? getenv("lab3PORT") : DEFAULT_PORT;
    int opt; // найденная опция
    int sock_fd; // файловый дескриптор сокета
    struct sockaddr_in server_addr; // структура с адресов сервера (айпи и порт)
    char *request; // строка запроса, к-ю отправим серверу
    char buf[512]; // буфер для чтения ответа от сервера
    ssize_t n; // количество байтов, прочитанных функцией read

    // разбираем опции командной строки
    while ((opt = getopt(argc, argv, "a:p:hv")) != -1) {
        switch (opt) {
            case 'a': // айпи
                addr = optarg;
                break;
            case 'p': // порт
                port_str = optarg;
                break;
            case 'h': // справка
                print_help();
                return 0;
            case 'v': // версия
                print_version();
                return 0;
            default: // если неизвестная опция
                return 1;
        }
    }

    if (optind >= argc) { // осталось ли выражение после разбора опций
        fprintf(stderr, "Ошибка: не указано выражение\n");
        print_help();
        return 1;
    }

    request = join_args(argc, argv, optind); // склеиваем все оставшиеся аргументы в одну строку запроса
    if (request == NULL) {
        fprintf(stderr, "Ошибка выделения памяти\n");
        return 1;
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0); // создаем TCP-сокет
    if (sock_fd < 0) {
        perror("socket");
        free(request);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr)); // обнуляем структуру адреса сервера
    server_addr.sin_family = AF_INET; // адрес IPv4
    server_addr.sin_port = htons((uint16_t)atoi(port_str)); // записываем порт
    // преобразуем айпи адрес из строки в бинарный вид
    if (inet_pton(AF_INET, addr, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Неверный IPv4-адрес\n");
        close(sock_fd);
        free(request);
        return 1;
    }
    // подключаемся к серверу
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        free(request);
        return 1;
    }
    // отправляем запрос серверу
    if (write_all(sock_fd, request) < 0) {
        perror("write");
        close(sock_fd);
        free(request);
        return 1;
    }
    // закрываем соединение в сторону сервера
    shutdown(sock_fd, SHUT_WR);
    // читаем ответ от сервера кусками
    while ((n = read(sock_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    close(sock_fd);
    free(request);
    return 0;
}
