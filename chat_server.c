/*
 * chat_server.c
 *
 * Servidor do chat TCP da disciplina, com um modo benchmark para medir
 * o throughput agregado de N clientes simultaneos.
 *
 * Modo chat (comportamento original):
 *     ./chat_server <porta>
 *
 * Modo benchmark:
 *     ./chat_server <porta> -n <clientes> [-r bytes_recv] [-i intervalo_ms]
 *                   [-o amostras.csv] [-q]
 *
 *     -n  quantidade de clientes esperados (obrigatorio no modo benchmark)
 *     -r  bytes pedidos em cada recv() (padrao: 262144 = 256 KiB)
 *     -i  intervalo de amostragem em ms (padrao: 1000)
 *     -o  grava a taxa de cada intervalo de amostragem em um arquivo CSV
 *     -q  imprime somente a linha RESULT
 *
 * send_all(), read_line(), broadcast(), handle_client() e o laco de
 * aceitacao do chat sao os do codigo original da disciplina. A unica
 * mudanca neles e o "%.31s" no snprintf() do nome em handle_client(): o
 * resultado e o mesmo de antes (o nome ja era cortado em 31 caracteres),
 * e o compilador deixa de emitir o aviso -Wformat-truncation.
 *
 * Compilacao: make   (ou gcc -O2 -Wall -Wextra -pthread chat_server.c -o chat_server)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Usadas apenas pelo modo benchmark. */
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100

typedef struct Client {
    int fd;
    char name[32];
    struct Client *next;
} Client;

Client *clients = NULL;
int num_clients = 0;

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Envia todos os bytes, mesmo se send() enviar parcialmente. */
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

/* Le uma linha terminada por '\n'. */
ssize_t read_line(int fd, char *buffer, size_t capacity) {
    size_t pos = 0;

    while (pos < capacity - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n == 0)
            return pos == 0 ? 0 : (ssize_t)pos;

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (c == '\n')
            break;

        if (c != '\r')
            buffer[pos++] = c;
    }

    buffer[pos] = '\0';
    return (ssize_t)pos;
}

/* Distribui uma mensagem aos demais clientes. */
void broadcast(const char *message, Client *sender) {
    pthread_mutex_lock(&clients_mutex);

    Client *current = clients;

    while (current != NULL) {
        if (current != sender) {
            if (send_all(current->fd, message,
                         strlen(message)) == -1) {
                fprintf(stderr,
                        "Falha ao enviar para %s.\n",
                        current->name);
            }
        }

        current = current->next;
    }

    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg) {
    Client *client = arg;
    char buffer[BUFFER_SIZE];
    char message[BUFFER_SIZE + 64];
    int registered = 0;

    /* A primeira linha recebida e o nome do usuario. */
    ssize_t n = read_line(client->fd, buffer, sizeof(buffer));

    if (n <= 0 || buffer[0] == '\0') {
        close(client->fd);
        free(client);
        return NULL;
    }

    snprintf(client->name, sizeof(client->name),
             "%.31s", buffer);

    pthread_mutex_lock(&clients_mutex);

    if (num_clients >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);

        const char *error = "Servidor cheio.\n";
        send_all(client->fd, error, strlen(error));
        close(client->fd);
        free(client);
        return NULL;
    }

    client->next = clients;
    clients = client;
    num_clients++;
    registered = 1;

    pthread_mutex_unlock(&clients_mutex);

    snprintf(message, sizeof(message),
             "*** %s entrou no chat. ***\n",
             client->name);
    broadcast(message, client);

    printf("Usuario conectado: %s\n", client->name);

    while ((n = read_line(client->fd, buffer,
                          sizeof(buffer))) > 0) {
        if (strcmp(buffer, "/quit") == 0)
            break;

        snprintf(message, sizeof(message),
                 "%s: %s\n", client->name, buffer);

        broadcast(message, client);
    }

    if (n == -1)
        perror("recv");

    if (registered) {
        pthread_mutex_lock(&clients_mutex);

        Client **current = &clients;

        while (*current != NULL && *current != client)
            current = &(*current)->next;

        if (*current == client) {
            *current = client->next;
            num_clients--;
        }

        pthread_mutex_unlock(&clients_mutex);

        snprintf(message, sizeof(message),
                 "*** %s saiu do chat. ***\n",
                 client->name);
        broadcast(message, NULL);
    }

    printf("Usuario desconectado: %s\n", client->name);

    close(client->fd);
    free(client);

    return NULL;
}

/* Modo chat: corpo do main() original, sem mudanca de comportamento. */
static int run_chat(int port) {
    int server_fd;
    struct sockaddr_in server_addr;

    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Chat TCP ativo na porta %d.\n", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &addr_len
        );

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        Client *client = malloc(sizeof(Client));

        if (client == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        client->fd = client_fd;
        client->name[0] = '\0';
        client->next = NULL;

        pthread_t tid;

        if (pthread_create(&tid, NULL,
                           handle_client, client) != 0) {
            fprintf(stderr, "Erro ao criar thread.\n");
            close(client_fd);
            free(client);
            continue;
        }

        pthread_detach(tid);
    }

    close(server_fd);
    pthread_mutex_destroy(&clients_mutex);

    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Modo benchmark                                                      */
/* ------------------------------------------------------------------ */

#define CACHE_LINE 64
#define BENCH_BACKLOG 128
#define BENCH_MAX_CLIENTS 1024
#define MAX_RECV_BYTES (64L * 1024 * 1024)

/*
 * Estado de uma thread receptora. Cada Slot ocupa exatamente uma linha
 * de cache, de modo que threads em nucleos diferentes nunca escrevem na
 * mesma linha (sem false sharing). So a propria thread escreve em bytes;
 * a thread de amostragem apenas le. Nao ha mutex no caminho de dados.
 */
typedef struct {
    _Atomic uint64_t bytes;   /* B_i: soma dos retornos de recv()       */
    uint64_t last_ns;         /* t_i: instante do ultimo recv() com dados */
    int fd;
    int index;
    char name[40];
} __attribute__((aligned(CACHE_LINE))) Slot;

_Static_assert(sizeof(Slot) == CACHE_LINE,
               "cada Slot deve ocupar uma linha de cache");

static Slot *slots;
static int bench_clients;
static size_t recv_size = 262144;
static long interval_ms = 1000;
static const char *samples_path;
static int quiet;

/* Largada sincronizada. */
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static int ready_count;
static int go_sent;
static uint64_t t0_ns;

/* Quantas threads ja detectaram o fim do fluxo. */
static atomic_int finished;

/* Thread de amostragem. */
static pthread_mutex_t sampler_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sampler_cond;
static int sampler_stop;
static double peak_mbps;

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Le os contadores agregados de CPU da linha "cpu" de /proc/stat. */
static int read_cpu(unsigned long long *total, unsigned long long *idle) {
    unsigned long long v[8] = {0};
    FILE *f = fopen("/proc/stat", "r");

    if (f == NULL)
        return -1;

    /* user nice system idle iowait irq softirq steal */
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5], &v[6], &v[7]);
    fclose(f);

    if (n < 4)
        return -1;

    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += v[i];

    *idle = v[3] + v[4];
    return 0;
}

static void *bench_receiver(void *arg) {
    Slot *slot = arg;
    char line[BUFFER_SIZE];

    /* Como no chat, a primeira linha e o nome do cliente. */
    ssize_t n = read_line(slot->fd, line, sizeof(line));

    if (n <= 0 || line[0] == '\0') {
        fprintf(stderr, "Cliente %d desconectou antes de se identificar.\n",
                slot->index + 1);
        exit(EXIT_FAILURE);
    }

    snprintf(slot->name, sizeof(slot->name), "%.39s", line);

    /* Avisa que esta pronto e espera o GO. */
    pthread_mutex_lock(&start_mutex);
    ready_count++;

    if (!quiet)
        printf("Cliente pronto: %s (%d/%d)\n",
               slot->name, ready_count, bench_clients);

    pthread_cond_broadcast(&start_cond);

    while (!go_sent)
        pthread_cond_wait(&start_cond, &start_mutex);

    uint64_t last = t0_ns;
    pthread_mutex_unlock(&start_mutex);

    char *buf = malloc(recv_size);

    if (buf == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    uint64_t total = 0;

    /* Caminho de dados: so soma bytes, sem parsing e sem broadcast. */
    while (1) {
        ssize_t r = recv(slot->fd, buf, recv_size, 0);

        if (r > 0) {
            total += (uint64_t)r;
            atomic_store_explicit(&slot->bytes, total,
                                  memory_order_relaxed);
            last = now_ns();
        } else if (r == 0) {
            break;                       /* shutdown(SHUT_WR) do cliente */
        } else if (errno != EINTR) {
            fprintf(stderr, "recv (%s): %s\n", slot->name, strerror(errno));
            break;
        }
    }

    slot->last_ns = last;
    atomic_fetch_add(&finished, 1);

    free(buf);
    close(slot->fd);
    return NULL;
}

/* Soma os N contadores a cada intervalo e guarda o pico. */
static void *bench_sampler(void *arg) {
    uint64_t step = (uint64_t)interval_ms * 1000000ULL;
    uint64_t prev_t = t0_ns;
    uint64_t prev_bytes = 0;
    uint64_t next = t0_ns + step;
    FILE *out = NULL;

    (void)arg;

    if (samples_path != NULL) {
        out = fopen(samples_path, "w");

        if (out == NULL)
            perror(samples_path);
        else
            fprintf(out, "inicio_s,fim_s,mbps,todos_transmitindo\n");
    }

    pthread_mutex_lock(&sampler_mutex);

    while (1) {
        struct timespec deadline = {
            .tv_sec = (time_t)(next / 1000000000ULL),
            .tv_nsec = (long)(next % 1000000000ULL)
        };
        int rc = 0;

        while (!sampler_stop && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&sampler_cond, &sampler_mutex,
                                        &deadline);

        int stop = sampler_stop;
        pthread_mutex_unlock(&sampler_mutex);

        /* O intervalo so conta para o pico se terminou pelo relogio
           (intervalo completo) e nenhum cliente havia encerrado o fluxo. */
        int all_sending = !stop && atomic_load(&finished) == 0;
        uint64_t t = now_ns();
        uint64_t bytes = 0;

        for (int i = 0; i < bench_clients; i++)
            bytes += atomic_load_explicit(&slots[i].bytes,
                                          memory_order_relaxed);

        double dt = (double)(t - prev_t) / 1e9;
        double mbps = dt > 0 ? 8.0 * (double)(bytes - prev_bytes) / (dt * 1e6)
                             : 0.0;

        if (all_sending && mbps > peak_mbps)
            peak_mbps = mbps;

        if (out != NULL)
            fprintf(out, "%.3f,%.3f,%.3f,%d\n",
                    (double)(prev_t - t0_ns) / 1e9,
                    (double)(t - t0_ns) / 1e9, mbps, all_sending);

        if (!quiet && !stop) {
            printf("  %6.1f s %14.1f Mbps%s\n",
                   (double)(t - t0_ns) / 1e9, mbps,
                   all_sending ? "" : "   (fim do fluxo)");
            fflush(stdout);
        }

        if (stop)
            break;

        prev_t = t;
        prev_bytes = bytes;
        next += step;

        /* Se a thread atrasou mais de um intervalo, realinha a grade. */
        if (next <= t)
            next = t + step;

        pthread_mutex_lock(&sampler_mutex);
    }

    if (out != NULL)
        fclose(out);

    return NULL;
}

static int run_benchmark(int port) {
    int server_fd;
    int opt = 1;
    struct sockaddr_in server_addr;
    pthread_t *receivers;
    pthread_t sampler;
    pthread_condattr_t cattr;
    unsigned long long cpu_total0 = 0, cpu_idle0 = 0;
    unsigned long long cpu_total1 = 0, cpu_idle1 = 0;

    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BENCH_BACKLOG) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    slots = aligned_alloc(CACHE_LINE, sizeof(Slot) * (size_t)bench_clients);
    receivers = calloc((size_t)bench_clients, sizeof(pthread_t));

    if (slots == NULL || receivers == NULL) {
        perror("malloc");
        close(server_fd);
        return EXIT_FAILURE;
    }

    memset(slots, 0, sizeof(Slot) * (size_t)bench_clients);

    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&sampler_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    if (!quiet)
        printf("Modo benchmark na porta %d: aguardando %d cliente(s), "
               "recv() de %zu bytes.\n", port, bench_clients, recv_size);

    /* Uma thread receptora por cliente aceito. */
    for (int i = 0; i < bench_clients; ) {
        int fd = accept(server_fd, NULL, NULL);

        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;

            perror("accept");
            return EXIT_FAILURE;
        }

        atomic_init(&slots[i].bytes, 0);
        slots[i].fd = fd;
        slots[i].index = i;

        if (pthread_create(&receivers[i], NULL,
                           bench_receiver, &slots[i]) != 0) {
            fprintf(stderr, "Erro ao criar thread.\n");
            return EXIT_FAILURE;
        }

        i++;
    }

    /* Nenhuma conexao alem das N esperadas. */
    close(server_fd);

    /* Largada: espera os N clientes identificados, le t0 e envia GO. */
    pthread_mutex_lock(&start_mutex);

    while (ready_count < bench_clients)
        pthread_cond_wait(&start_cond, &start_mutex);

    int cpu_ok = read_cpu(&cpu_total0, &cpu_idle0) == 0;

    t0_ns = now_ns();

    for (int i = 0; i < bench_clients; i++) {
        if (send_all(slots[i].fd, "GO\n", 3) == -1)
            fprintf(stderr, "Falha ao enviar GO para %s.\n", slots[i].name);
    }

    go_sent = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);

    if (!quiet)
        printf("GO enviado a %d cliente(s).\n", bench_clients);

    int sampler_ok = pthread_create(&sampler, NULL, bench_sampler, NULL) == 0;

    if (!sampler_ok)
        fprintf(stderr, "Aviso: amostragem nao iniciada; pico indisponivel.\n");

    for (int i = 0; i < bench_clients; i++)
        pthread_join(receivers[i], NULL);

    cpu_ok = cpu_ok && read_cpu(&cpu_total1, &cpu_idle1) == 0;

    pthread_mutex_lock(&sampler_mutex);
    sampler_stop = 1;
    pthread_cond_signal(&sampler_cond);
    pthread_mutex_unlock(&sampler_mutex);

    if (sampler_ok)
        pthread_join(sampler, NULL);

    /* T = max(t_i) - t0 e taxa agregada = 8 * soma(B_i) / (T * 10^6). */
    uint64_t total = 0;
    uint64_t t_last = t0_ns;
    double sum_x = 0.0;
    double sum_x2 = 0.0;

    if (!quiet)
        printf("Por cliente:\n");

    for (int i = 0; i < bench_clients; i++) {
        uint64_t b = atomic_load(&slots[i].bytes);
        uint64_t ti = slots[i].last_ns;
        double secs = (double)(ti - t0_ns) / 1e9;
        double x = secs > 0 ? 8.0 * (double)b / (secs * 1e6) : 0.0;

        total += b;

        if (ti > t_last)
            t_last = ti;

        sum_x += x;
        sum_x2 += x * x;

        if (!quiet)
            printf("  %-16s %16llu bytes %14.1f Mbps\n",
                   slots[i].name, (unsigned long long)b, x);
    }

    double window = (double)(t_last - t0_ns) / 1e9;
    double mbps = window > 0 ? 8.0 * (double)total / (window * 1e6) : 0.0;
    double jain = sum_x2 > 0 ? (sum_x * sum_x) /
                               ((double)bench_clients * sum_x2) : 0.0;
    double cpu = -1.0;

    if (cpu_ok && cpu_total1 > cpu_total0)
        cpu = 100.0 * (1.0 - (double)(cpu_idle1 - cpu_idle0) /
                             (double)(cpu_total1 - cpu_total0));

    printf("RESULT clients=%d bytes=%llu window_s=%.6f mbps=%.3f "
           "peak_mbps=%.3f cpu=%.1f jain=%.4f\n",
           bench_clients, (unsigned long long)total, window, mbps,
           peak_mbps, cpu, jain);
    fflush(stdout);

    free(slots);
    free(receivers);
    pthread_cond_destroy(&sampler_cond);

    return EXIT_SUCCESS;
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

static void usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s <porta>\n"
            "     %s <porta> -n <clientes> [-r bytes_recv] [-i intervalo_ms]\n"
            "            [-o amostras.csv] [-q]\n",
            prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int port = (int)parse_long(argv[1], 1, 65535, "<porta>");

    /* Sem opcoes: chat original. */
    if (argc == 2)
        return run_chat(port);

    int opt;
    opterr = 0;

    /* getopt() trata argv[1] (a porta) como se fosse o nome do programa. */
    while ((opt = getopt(argc - 1, argv + 1, ":n:r:i:o:q")) != -1) {
        switch (opt) {
        case 'n':
            bench_clients = (int)parse_long(optarg, 1, BENCH_MAX_CLIENTS, "-n");
            break;
        case 'r':
            recv_size = (size_t)parse_long(optarg, 1, MAX_RECV_BYTES, "-r");
            break;
        case 'i':
            interval_ms = parse_long(optarg, 10, 60000, "-i");
            break;
        case 'o':
            samples_path = optarg;
            break;
        case 'q':
            quiet = 1;
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

    if (optind < argc - 1) {
        fprintf(stderr, "Argumento inesperado: %s\n", argv[optind + 1]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (bench_clients == 0) {
        fprintf(stderr, "O modo benchmark exige -n <clientes>.\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    return run_benchmark(port);
}
