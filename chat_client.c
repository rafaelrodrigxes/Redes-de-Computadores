/*
 * chat_client.c
 *
 * Cliente do chat TCP da disciplina, com um modo de transmissao continua
 * para o benchmark de throughput.
 *
 * Modo chat (comportamento original):
 *     ./chat_client <IP> <porta>
 *
 * Modo benchmark:
 *     ./chat_client <IP> <porta> -t <segundos> [-b bytes_bloco] [-n nome]
 *
 *     -t  duracao da transmissao em segundos (obrigatorio no modo benchmark)
 *     -b  bytes por send() (padrao: 131072 = 128 KiB)
 *     -n  nome enviado na identificacao (padrao: cliente-<pid>)
 *
 * send_all(), receive_messages() e o fluxo do chat sao os do codigo
 * original da disciplina.
 *
 * Compilacao: make   (ou gcc -O2 -Wall -Wextra -pthread chat_client.c -o chat_client)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Usadas apenas pelo modo benchmark. */
#include <ctype.h>
#include <stdint.h>
#include <time.h>

#define BUFFER_SIZE 1024

int sockfd;
pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent,
                         MSG_NOSIGNAL);

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

void *receive_messages(void *arg) {
    char buffer[BUFFER_SIZE];
    size_t pos = 0;

    (void)arg;

    while (1) {
        char c;
        ssize_t n = recv(sockfd, &c, 1, 0);

        if (n == 0) {
            pthread_mutex_lock(&output_mutex);
            printf("\nConexao encerrada pelo servidor.\n");
            fflush(stdout);
            pthread_mutex_unlock(&output_mutex);
            break;
        }

        if (n == -1) {
            if (errno == EINTR)
                continue;

            pthread_mutex_lock(&output_mutex);
            perror("recv");
            pthread_mutex_unlock(&output_mutex);
            break;
        }

        if (c == '\n') {
            buffer[pos] = '\0';

            pthread_mutex_lock(&output_mutex);
            printf("\r%s\n", buffer);
            fflush(stdout);
            pthread_mutex_unlock(&output_mutex);

            pos = 0;
        } else if (c != '\r') {
            if (pos < sizeof(buffer) - 1)
                buffer[pos++] = c;
        }
    }

    return NULL;
}

/* Modo chat: corpo do main() original, sem mudanca de comportamento. */
static int run_chat(const char *ip, int port) {
    struct sockaddr_in server_addr;
    char name[32];
    char buffer[BUFFER_SIZE];
    pthread_t receiver;

    signal(SIGPIPE, SIG_IGN);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip,
                  &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Endereco IPv4 invalido.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Seu nome: ");

    if (fgets(name, sizeof(name), stdin) == NULL) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    name[strcspn(name, "\r\n")] = '\0';

    if (name[0] == '\0' ||
        strchr(name, '\n') != NULL) {
        fprintf(stderr, "Nome invalido.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (send_all(sockfd, name, strlen(name)) == -1 ||
        send_all(sockfd, "\n", 1) == -1) {
        perror("send");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (pthread_create(&receiver, NULL,
                       receive_messages, NULL) != 0) {
        fprintf(stderr, "Erro ao criar thread receptora.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Conectado! Digite /quit para sair.\n");

    while (1) {
        pthread_mutex_lock(&output_mutex);
        printf("> ");
        fflush(stdout);
        pthread_mutex_unlock(&output_mutex);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            break;

        if (strcmp(buffer, "/quit\n") == 0 ||
            strcmp(buffer, "/quit") == 0) {
            send_all(sockfd, "/quit\n", 6);
            break;
        }

        size_t len = strlen(buffer);

        if (len > 0 && buffer[len - 1] != '\n') {
            int c;
            while ((c = getchar()) != '\n' && c != EOF)
                ;
            fprintf(stderr, "Mensagem muito longa.\n");
            continue;
        }

        if (send_all(sockfd, buffer, len) == -1) {
            perror("send");
            break;
        }
    }

    shutdown(sockfd, SHUT_RDWR);
    pthread_join(receiver, NULL);
    close(sockfd);

    pthread_mutex_destroy(&output_mutex);

    printf("Cliente encerrado.\n");

    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Modo benchmark                                                      */
/* ------------------------------------------------------------------ */

#define DEFAULT_BLOCK 131072
#define MAX_BLOCK_BYTES (64L * 1024 * 1024)
#define CONNECT_RETRY_NS 10000000000ULL  /* 10 s */
#define MAX_NAME_LEN 31

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/*
 * Conecta ao servidor. Se ele ainda nao estiver aguardando conexoes,
 * tenta de novo a cada 100 ms, por ate 10 s.
 */
static int connect_with_retry(const struct sockaddr_in *addr) {
    uint64_t deadline = now_ns() + CONNECT_RETRY_NS;

    while (1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd == -1) {
            perror("socket");
            return -1;
        }

        if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0)
            return fd;

        int err = errno;
        close(fd);

        int transient = err == ECONNREFUSED || err == ECONNRESET ||
                        err == ETIMEDOUT || err == EINTR || err == EAGAIN;

        if (!transient || now_ns() >= deadline) {
            fprintf(stderr, "connect: %s\n", strerror(err));
            return -1;
        }

        sleep_ms(100);
    }
}

/* Espera a linha "GO" do servidor, lida um byte por vez. */
static int wait_go(int fd) {
    char line[16];
    size_t pos = 0;

    while (pos < sizeof(line) - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n == 0)
            return -1;

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (c == '\n')
            break;

        if (c != '\r')
            line[pos++] = c;
    }

    line[pos] = '\0';
    return strcmp(line, "GO") == 0 ? 0 : -1;
}

static int run_benchmark(const char *ip, int port, double seconds,
                         size_t block, const char *name) {
    struct sockaddr_in server_addr;

    signal(SIGPIPE, SIG_IGN);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Endereco IPv4 invalido.\n");
        return EXIT_FAILURE;
    }

    int fd = connect_with_retry(&server_addr);

    if (fd == -1)
        return EXIT_FAILURE;

    /* Identificacao igual a do chat: a primeira linha e o nome. */
    if (send_all(fd, name, strlen(name)) == -1 ||
        send_all(fd, "\n", 1) == -1) {
        perror("send");
        close(fd);
        return EXIT_FAILURE;
    }

    if (wait_go(fd) == -1) {
        fprintf(stderr, "O servidor encerrou a conexao sem enviar GO.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    char *data = malloc(block);

    if (data == NULL) {
        perror("malloc");
        close(fd);
        return EXIT_FAILURE;
    }

    memset(data, 'x', block);

    /* Reenvia o mesmo bloco, sem pausa, durante -t segundos. */
    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)(seconds * 1e9 + 0.5);
    uint64_t now = start;
    uint64_t total = 0;
    int failed = 0;

    while (now < end) {
        ssize_t n = send(fd, data, block, MSG_NOSIGNAL);

        if (n > 0) {
            total += (uint64_t)n;
        } else if (n == 0 || errno != EINTR) {
            perror("send");
            failed = 1;
            break;
        }

        now = now_ns();
    }

    double elapsed = (double)(now - start) / 1e9;

    /* Fim do fluxo: o servidor ve recv() == 0 e fecha a conexao. */
    shutdown(fd, SHUT_WR);

    char drain[256];
    ssize_t r;

    while ((r = recv(fd, drain, sizeof(drain), 0)) > 0 ||
           (r == -1 && errno == EINTR))
        ;

    close(fd);
    free(data);

    printf("CLIENT name=%s bytes=%llu seconds=%.3f mbps=%.3f\n",
           name, (unsigned long long)total, elapsed,
           elapsed > 0 ? 8.0 * (double)total / (elapsed * 1e6) : 0.0);
    fflush(stdout);

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static long parse_long(const char *text, long min, long max,
                       const char *what) {
    char *end;

    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        value < min || value > max) {
        fprintf(stderr, "Valor invalido para %s: '%s' (aceito: %ld a %ld).\n",
                what, text, min, max);
        exit(EXIT_FAILURE);
    }

    return value;
}

static double parse_seconds(const char *text) {
    char *end;

    errno = 0;
    double value = strtod(text, &end);

    /* A comparacao negada tambem rejeita NaN e infinito. */
    if (errno != 0 || end == text || *end != '\0' ||
        !(value >= 0.001 && value <= 86400.0)) {
        fprintf(stderr, "Valor invalido para -t: '%s' (aceito: 0.001 a 86400).\n",
                text);
        exit(EXIT_FAILURE);
    }

    return value;
}

/* Nome de 1 a 31 caracteres visiveis, sem espacos e sem '='. */
static int valid_name(const char *name) {
    size_t len = strlen(name);

    if (len == 0 || len > MAX_NAME_LEN)
        return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (!isgraph(c) || c == '=')
            return 0;
    }

    return 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s <IP> <porta>\n"
            "     %s <IP> <porta> -t <segundos> [-b bytes_bloco] [-n nome]\n",
            prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int port = (int)parse_long(argv[2], 1, 65535, "<porta>");

    /* Sem opcoes: chat original. */
    if (argc == 3)
        return run_chat(argv[1], port);

    double seconds = 0.0;
    size_t block = DEFAULT_BLOCK;
    char name[MAX_NAME_LEN + 1];
    int opt;

    snprintf(name, sizeof(name), "cliente-%ld", (long)getpid());
    opterr = 0;

    /* getopt() trata argv[2] (a porta) como se fosse o nome do programa. */
    while ((opt = getopt(argc - 2, argv + 2, ":t:b:n:")) != -1) {
        switch (opt) {
        case 't':
            seconds = parse_seconds(optarg);
            break;
        case 'b':
            block = (size_t)parse_long(optarg, 1, MAX_BLOCK_BYTES, "-b");
            break;
        case 'n':
            if (!valid_name(optarg)) {
                fprintf(stderr, "Nome invalido: use de 1 a %d caracteres, "
                        "sem espacos e sem '='.\n", MAX_NAME_LEN);
                return EXIT_FAILURE;
            }
            strcpy(name, optarg);
            break;
        case ':':
            fprintf(stderr, "A opcao -%c exige um valor.\n", optopt);
            usage(argv[0]);
            return EXIT_FAILURE;
        default:
            fprintf(stderr, "Opcao desconhecida: -%c\n", optopt);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc - 2) {
        fprintf(stderr, "Argumento inesperado: %s\n", argv[optind + 2]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (seconds <= 0.0) {
        fprintf(stderr, "O modo benchmark exige -t <segundos>.\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    return run_benchmark(argv[1], port, seconds, block, name);
}
