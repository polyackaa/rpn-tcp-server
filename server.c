#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define PROGRAM_NAME "server"

typedef enum {
    TOK_NUMBER,
    TOK_OPERATOR,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_END,
    TOK_INVALID
} token_type_t;

typedef struct {
    token_type_t type;
    char *text;
    char op;
} token_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} string_buf_t;

// флаг, который становится 1 при получении SIGINT/SIGTERM/SIGQUIT
static volatile sig_atomic_t stop_server = 0;
static volatile sig_atomic_t need_stats = 0;
struct stats_block {
    int success_count;
    int error_count;
};

static struct stats_block *stats = NULL;
// время запуска сервера для подсчета времени работы
static time_t start_time;
// путь к лог-файлу сервера
static const char *log_path = DEFAULT_LOGFILE;
static int wait_seconds = 0;

// обработчик сигналов завершения и SIGUSR1
static void handle_signal(int signo) {
    if (signo == SIGUSR1) {
        need_stats = 1;
    } else {
        stop_server = 1;
    }
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    // указываем функцию, которая будет вызвана при поступлении сигнала
    sigemptyset(&sa.sa_mask);
    // во время работы обработчика дополнительные сигналы не блокируются
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
}

// запись сообщения в лог-файл с временной меткой
static void log_message(const char *fmt, ...) {
    FILE *log_file;
    time_t now;
    struct tm tm_now;
    char time_buf[32];
    va_list args;

    log_file = fopen(log_path, "a");
    if (log_file == NULL) {
        return;
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(time_buf, sizeof(time_buf), "%d.%m.%y %H:%M:%S", &tm_now);

    fprintf(log_file, "[%s] ", time_buf);

    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);

    fprintf(log_file, "\n");
    fclose(log_file);
}

static void print_stats(void) {
    time_t now = time(NULL);
    long uptime = (long)(now - start_time);
    fprintf(stderr, "Статистика: время работы %ld сек., успешных запросов %d, ошибок %d\n",
            uptime, stats->success_count, stats->error_count);
    log_message("Статистика: время работы %ld сек., успешных запросов %d, ошибок %d",
                uptime, stats->success_count, stats->error_count);
}

// вывод информации о программе: имя, автор, группа и вариант
static void print_version(void) {
    printf("%s\n", PROGRAM_NAME);
    printf("Автор: Спицына Полина\n");
    printf("Назначение: преобразование выражений в постфиксную запись\n");
    printf("Транспорт: TCP\n");
    printf("Многозадачность: fork\n");
}

// вывод справки по опциям сервера
static void print_help(void) {
    printf("Использование: %s [опции]\n", PROGRAM_NAME);
    printf("Опции:\n");
    printf("  -a ip          IPv4-адрес сервера\n");
    printf("  -p port        порт сервера\n");
    printf("  -l path        путь к лог-файлу\n");
    printf("  -w N           задержка обработки запроса в секундах\n");
    printf("  -d             запуск в режиме демона\n");
    printf("  -h             справка\n");
    printf("  -v             версия\n");
}

static int sb_init(string_buf_t *sb) {
    sb->cap = 128;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    if (sb->data == NULL) {
        return -1;
    }
    sb->data[0] = '\0';
    return 0;
}

static void sb_free(string_buf_t *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_append(string_buf_t *sb, const char *text) {
    size_t add_len = strlen(text);
    size_t need = sb->len + add_len + 1;
    char *tmp;

    if (need > sb->cap) {
        size_t new_cap = sb->cap;
        while (new_cap < need) {
            new_cap *= 2;
        }
        tmp = realloc(sb->data, new_cap);
        if (tmp == NULL) {
            return -1;
        }
        sb->data = tmp;
        sb->cap = new_cap;
    }

    memcpy(sb->data + sb->len, text, add_len + 1);
    sb->len += add_len;
    return 0;
}

static int sb_append_char(string_buf_t *sb, char c) {
    char tmp[2];
    tmp[0] = c;
    tmp[1] = '\0';
    return sb_append(sb, tmp);
}

// определение приоритета арифметической операции
static int precedence(char op) {
    if (op == '+' || op == '-') {
        return 1;
    }
    if (op == '*' || op == '/') {
        return 2;
    }
    return 0;
}

// проверка, является ли символ арифметическим оператором
static int is_operator(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/';
}

static token_t next_token(const char *s, size_t *pos, int expect_operand) {
    token_t tok;
    size_t start;
    char *endptr;

    tok.type = TOK_INVALID;
    tok.text = NULL;
    tok.op = '\0';

    while (s[*pos] != '\0' && s[*pos] != '\n' && isspace((unsigned char)s[*pos])) {
        (*pos)++;
    }

    if (s[*pos] == '\0' || s[*pos] == '\n') {
        tok.type = TOK_END;
        return tok;
    }

    if (s[*pos] == '(') {
        tok.type = TOK_LPAREN;
        (*pos)++;
        return tok;
    }

    if (s[*pos] == ')') {
        tok.type = TOK_RPAREN;
        (*pos)++;
        return tok;
    }

    if (is_operator(s[*pos]) && !(expect_operand && (s[*pos] == '+' || s[*pos] == '-') &&
        (isdigit((unsigned char)s[*pos + 1]) || s[*pos + 1] == '.'))) {
        tok.type = TOK_OPERATOR;
        tok.op = s[*pos];
        (*pos)++;
        return tok;
    }

    start = *pos;
    errno = 0;
    (void)strtod(s + start, &endptr);
    if (endptr == s + start || errno == ERANGE) {
        tok.type = TOK_INVALID;
        return tok;
    }

    *pos = (size_t)(endptr - s);
    tok.text = malloc(*pos - start + 1);
    if (tok.text == NULL) {
        tok.type = TOK_INVALID;
        return tok;
    }
    memcpy(tok.text, s + start, *pos - start);
    tok.text[*pos - start] = '\0';
    tok.type = TOK_NUMBER;
    return tok;
}

static int append_output_token(string_buf_t *out, const char *text, int *first) {
    if (!*first) {
        if (sb_append_char(out, ' ') < 0) {
            return -1;
        }
    }
    if (sb_append(out, text) < 0) {
        return -1;
    }
    *first = 0;
    return 0;
}

// перевод выражения из инфиксной записи в постфиксную
static int infix_to_postfix(const char *expr, char **answer) {
    string_buf_t out;
    char *ops = NULL;
    size_t ops_len = 0;
    size_t ops_cap = 16;
    size_t pos = 0;
    int first = 1;
    int expect_operand = 1;
    int rc = ERR_INTERNAL;

    if (sb_init(&out) < 0) {
        return ERR_MEMORY;
    }

    ops = malloc(ops_cap);
    if (ops == NULL) {
        sb_free(&out);
        return ERR_MEMORY;
    }

    for (;;) {
        token_t tok = next_token(expr, &pos, expect_operand);

        if (tok.type == TOK_INVALID) {
            rc = ERR_BAD_REQUEST;
            break;
        }

        if (tok.type == TOK_END) {
            if (expect_operand) {
                rc = ERR_BAD_REQUEST;
                break;
            }
            while (ops_len > 0) {
                char op = ops[--ops_len];
                if (op == '(') {
                    rc = ERR_BAD_REQUEST;
                    goto cleanup;
                }
                if (append_output_token(&out, (char[]){op, '\0'}, &first) < 0) {
                    rc = ERR_MEMORY;
                    goto cleanup;
                }
            }
            if (sb_append_char(&out, '\n') < 0) {
                rc = ERR_MEMORY;
                break;
            }
            *answer = out.data;
            free(ops);
            return 0;
        }

        if (tok.type == TOK_NUMBER) {
            if (!expect_operand) {
                free(tok.text);
                rc = ERR_BAD_REQUEST;
                break;
            }
            if (append_output_token(&out, tok.text, &first) < 0) {
                free(tok.text);
                rc = ERR_MEMORY;
                break;
            }
            free(tok.text);
            expect_operand = 0;
            continue;
        }

        if (tok.type == TOK_LPAREN) {
            if (!expect_operand) {
                rc = ERR_BAD_REQUEST;
                break;
            }
            if (ops_len == ops_cap) {
                char *tmp;
                ops_cap *= 2;
                tmp = realloc(ops, ops_cap);
                if (tmp == NULL) {
                    rc = ERR_MEMORY;
                    break;
                }
                ops = tmp;
            }
            ops[ops_len++] = '(';
            expect_operand = 1;
            continue;
        }

        if (tok.type == TOK_RPAREN) {
            int found_left = 0;
            if (expect_operand) {
                rc = ERR_BAD_REQUEST;
                break;
            }
            while (ops_len > 0) {
                char op = ops[--ops_len];
                if (op == '(') {
                    found_left = 1;
                    break;
                }
                if (append_output_token(&out, (char[]){op, '\0'}, &first) < 0) {
                    rc = ERR_MEMORY;
                    goto cleanup;
                }
            }
            if (!found_left) {
                rc = ERR_BAD_REQUEST;
                break;
            }
            expect_operand = 0;
            continue;
        }

        if (tok.type == TOK_OPERATOR) {
            if (expect_operand) {
                rc = ERR_BAD_REQUEST;
                break;
            }
            while (ops_len > 0 && ops[ops_len - 1] != '(' &&
                   precedence(ops[ops_len - 1]) >= precedence(tok.op)) {
                char op = ops[--ops_len];
                if (append_output_token(&out, (char[]){op, '\0'}, &first) < 0) {
                    rc = ERR_MEMORY;
                    goto cleanup;
                }
            }
            if (ops_len == ops_cap) {
                char *tmp;
                ops_cap *= 2;
                tmp = realloc(ops, ops_cap);
                if (tmp == NULL) {
                    rc = ERR_MEMORY;
                    break;
                }
                ops = tmp;
            }
            ops[ops_len++] = tok.op;
            expect_operand = 1;
        }
    }

cleanup:
    sb_free(&out);
    free(ops);
    return rc;
}

static char *read_request(int fd) {
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 128;

    buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }

    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        if (len + 1 >= cap) {
            char *tmp;
            cap *= 2;
            tmp = realloc(buf, cap);
            if (tmp == NULL) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
        buf[len++] = c;
        if (c == '\n') {
            break;
        }
    }

    buf[len] = '\0';
    return buf;
}

static void write_all(int fd, const char *s) {
    size_t len = strlen(s);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, s + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        sent += (size_t)n;
    }
}

static void serve_client(int client_fd) {
    char *request;
    char *answer = NULL;
    int rc;

    if (wait_seconds > 0) {
        sleep((unsigned int)wait_seconds);
        // имитируем длительную обработку запроса по опции -w
    }

    request = read_request(client_fd);
    if (request == NULL || request[0] == '\0') {
        write_all(client_fd, "ERROR 1\n");
        stats->error_count++;
        free(request);
        close(client_fd);
            // родительский процесс закрывает client_fd, потому что клиентом занимается дочерний процесс
        return;
    }

    log_message("Поступил запрос: %s", request);
    rc = infix_to_postfix(request, &answer);
    if (rc == 0) {
        write_all(client_fd, answer);
        stats->success_count++;
        log_message("Запрос успешно обработан");
    } else {
        char err_buf[32];
        snprintf(err_buf, sizeof(err_buf), "ERROR %d\n", rc);
        write_all(client_fd, err_buf);
        stats->error_count++;
        log_message("Ошибка обработки запроса: %d", rc);
    }

    free(answer);
    free(request);
    close(client_fd);
            // родительский процесс закрывает client_fd, потому что клиентом занимается дочерний процесс
}

// перевод сервера в режим демона
static int daemonize(void) {
    pid_t pid = fork();
        // fork создает дочерний процесс для обслуживания конкретного клиента
    int fd;

    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    pid = fork();
        // fork создает дочерний процесс для обслуживания конкретного клиента
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    umask(0);
    if (chdir("/") < 0) {
    exit(EXIT_FAILURE);
    }

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    return 0;
}

// главная функция сервера
int main(int argc, char *argv[]) {
    const char *addr = getenv("SERVER_ADDR") ? getenv("SERVER_ADDR") : DEFAULT_ADDR;
    const char *port_str = getenv("SERVER_PORT") ? getenv("SERVER_PORT") : DEFAULT_PORT;
    const char *env_log = getenv("SERVER_LOGFILE");
    int daemon_mode = 0;
    int server_fd;
    struct sockaddr_in server_addr;
    int opt;
    int yes = 1;

    if (env_log != NULL) {
        log_path = env_log;
    }
    if (getenv("SERVER_WAIT") != NULL) {
        wait_seconds = atoi(getenv("SERVER_WAIT"));
    }

    while ((opt = getopt(argc, argv, "a:p:l:w:dhv")) != -1) {
        switch (opt) {
            case 'a':
                addr = optarg;
                break;
            case 'p':
                port_str = optarg;
                break;
            case 'l':
                log_path = optarg;
                break;
            case 'w':
                wait_seconds = atoi(optarg);
                break;
            case 'd':
                daemon_mode = 1;
                break;
            case 'h':
                print_help();
                return 0;
            case 'v':
                print_version();
                return 0;
            default:
                return 1;
        }
    }

    if (daemon_mode && daemonize() < 0) {
        fprintf(stderr, "Ошибка запуска демона\n");
        return 1;
    }

    stats = mmap(NULL, sizeof(*stats), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    // mmap создает участок памяти, общий для родительского и дочерних процессов после fork
    if (stats == MAP_FAILED) {
        // если общую память создать не удалось, сервер не сможет корректно считать статистику
        perror("mmap");
        return 1;
    }
    stats->success_count = 0;
    stats->error_count = 0;

    start_time = time(NULL);
    install_signals();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)atoi(port_str));

    if (inet_pton(AF_INET, addr, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Неверный IPv4-адрес\n");
        close(server_fd);
        return 1;
    }

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, MAX_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    log_message("Сервер запущен на %s:%s", addr, port_str);

    while (!stop_server) {
        int client_fd;
        pid_t pid;

        if (need_stats) {
            print_stats();
            need_stats = 0;
        }

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        log_message("Поступило новое соединение");

        pid = fork();
        // fork создает дочерний процесс для обслуживания конкретного клиента
        if (pid < 0) {
            write_all(client_fd, "ERROR 3\n");
            close(client_fd);
            // родительский процесс закрывает client_fd, потому что клиентом занимается дочерний процесс
            stats->error_count++;
            continue;
        }

        if (pid == 0) {
            // код внутри этого блока выполняется только в дочернем процессе
            close(server_fd);
            serve_client(client_fd);
            exit(0);
        }

        close(client_fd);
            // родительский процесс закрывает client_fd, потому что клиентом занимается дочерний процесс
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            // убираем завершившиеся дочерние процессы, чтобы не оставались zombie-процессы
        }
    }

    close(server_fd);
    while (waitpid(-1, NULL, WNOHANG) > 0) {
            // убираем завершившиеся дочерние процессы, чтобы не оставались zombie-процессы
    }
    log_message("Сервер завершил работу");
    munmap(stats, sizeof(*stats));
    return 0;
}