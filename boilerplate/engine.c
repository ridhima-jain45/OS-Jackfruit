// ===== FINAL ENGINE.C (FULL IMPLEMENTATION) =====

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_BUFFER_CAPACITY 32
#define LOG_CHUNK_SIZE 4096

// ================= DATA =================

typedef enum {
    RUNNING,
    STOPPED,
    EXITED,
    KILLED
} state_t;

typedef struct container {
    char id[32];
    pid_t pid;
    state_t state;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container *next;
} container_t;

typedef struct {
    char id[32];
    size_t len;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t buf[LOG_BUFFER_CAPACITY];
    int head, tail, count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty;
} buffer_t;

typedef struct {
    int fd;
    char id[32];
} producer_arg_t;

// ================= GLOBAL =================

buffer_t logbuf;
container_t *containers = NULL;
pthread_mutex_t meta_lock = PTHREAD_MUTEX_INITIALIZER;

// ================= BUFFER =================

int buffer_push(buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->lock);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->lock);

    if (b->shutdown) {
        pthread_mutex_unlock(&b->lock);
        return -1;
    }

    b->buf[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

int buffer_pop(buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->lock);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->lock);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->lock);
        return -1;
    }

    *item = b->buf[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

// ================= LOGGER =================

void *logger_thread(void *arg) {
    (void)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (buffer_pop(&logbuf, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.len, f);
            fclose(f);
        }
    }
    return NULL;
}

// ================= PRODUCER =================

void *producer_thread(void *arg) {
    producer_arg_t *p = arg;
    char buf[LOG_CHUNK_SIZE];

    while (1) {
        int n = read(p->fd, buf, sizeof(buf));
        if (n <= 0) break;

        log_item_t item;
        strcpy(item.id, p->id);
        memcpy(item.data, buf, n);
        item.len = n;

        buffer_push(&logbuf, &item);
    }

    close(p->fd);
    free(p);
    return NULL;
}

// ================= CONTAINER =================

int child_fn(void *arg) {
    char **cfg = arg;

    sethostname(cfg[0], strlen(cfg[0]));

    chroot(cfg[1]);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    execl("/bin/sh", "sh", "-c", cfg[2], NULL);
    perror("exec");
    return 1;
}

// ================= METADATA =================

void add_container(char *id, pid_t pid) {
    container_t *c = malloc(sizeof(container_t));
    strcpy(c->id, id);
    c->pid = pid;
    c->state = RUNNING;
    snprintf(c->log_path, PATH_MAX, "logs/%s.log", id);

    pthread_mutex_lock(&meta_lock);
    c->next = containers;
    containers = c;
    pthread_mutex_unlock(&meta_lock);
}

container_t *find_container(char *id) {
    container_t *c = containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

// ================= SIGNAL =================

void reap_children() {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&meta_lock);
        container_t *c = containers;

        while (c) {
            if (c->pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->state = KILLED;
                    c->exit_signal = WTERMSIG(status);
                }
            }
            c = c->next;
        }
        pthread_mutex_unlock(&meta_lock);
    }
}

// ================= SUPERVISOR =================

void handle_start(int client) {
    char id[32], rootfs[PATH_MAX], cmd[256];
    read(client, id, sizeof(id));
    read(client, rootfs, sizeof(rootfs));
    read(client, cmd, sizeof(cmd));

    int pipefd[2];
    pipe(pipefd);

    char *stack = malloc(STACK_SIZE);

    char **cfg = malloc(3 * sizeof(char *));
    cfg[0] = strdup(id);
    cfg[1] = strdup(rootfs);
    cfg[2] = strdup(cmd);

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    close(pipefd[1]);

    add_container(id, pid);

    producer_arg_t *p = malloc(sizeof(producer_arg_t));
    p->fd = pipefd[0];
    strcpy(p->id, id);

    pthread_t t;
    pthread_create(&t, NULL, producer_thread, p);

    write(client, "OK", 2);
}

void handle_ps(int client) {
    char buffer[1024] = {0};

    pthread_mutex_lock(&meta_lock);
    container_t *c = containers;

    while (c) {
        char line[256];
        snprintf(line, sizeof(line),
                 "%s PID=%d STATE=%d\n",
                 c->id, c->pid, c->state);
        strcat(buffer, line);
        c = c->next;
    }
    pthread_mutex_unlock(&meta_lock);

    write(client, buffer, strlen(buffer));
}

void handle_stop(int client) {
    char id[32];
    read(client, id, sizeof(id));

    pthread_mutex_lock(&meta_lock);
    container_t *c = find_container(id);
    if (c) {
        kill(c->pid, SIGTERM);
        c->state = STOPPED;
    }
    pthread_mutex_unlock(&meta_lock);

    write(client, "STOPPED", 7);
}

void handle_logs(int client) {
    char id[32];
    read(client, id, sizeof(id));

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, id);

    FILE *f = fopen(path, "r");
    if (!f) {
        write(client, "NO LOGS\n", 8);
        return;
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        write(client, buf, strlen(buf));
    }
    fclose(f);
}

// ================= MAIN =================

int run_supervisor() {
    unlink(CONTROL_PATH);

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, 10);

    pthread_mutex_init(&logbuf.lock, NULL);
    pthread_cond_init(&logbuf.not_full, NULL);
    pthread_cond_init(&logbuf.not_empty, NULL);

    pthread_t logger;
    pthread_create(&logger, NULL, logger_thread, NULL);

    printf("[Supervisor Running]\n");

    while (1) {
        reap_children();

        int client = accept(server, NULL, NULL);

        int cmd;
        read(client, &cmd, sizeof(int));

        if (cmd == 1) handle_start(client);
        else if (cmd == 2) handle_ps(client);
        else if (cmd == 3) handle_stop(client);
        else if (cmd == 4) handle_logs(client);

        close(client);
    }
}

// ================= CLIENT =================

void send_cmd(int cmd, char *a, char *b, char *c) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    write(sock, &cmd, sizeof(int));
    if (a) write(sock, a, 32);
    if (b) write(sock, b, PATH_MAX);
    if (c) write(sock, c, 256);

    char buf[1024];
    int n;
    while ((n = read(sock, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }

    close(sock);
}

// ================= ENTRY =================

int main(int argc, char *argv[]) {

    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    if (strcmp(argv[1], "start") == 0)
        send_cmd(1, argv[2], argv[3], argv[4]);

    else if (strcmp(argv[1], "ps") == 0)
        send_cmd(2, NULL, NULL, NULL);

    else if (strcmp(argv[1], "stop") == 0)
        send_cmd(3, argv[2], NULL, NULL);

    else if (strcmp(argv[1], "logs") == 0)
        send_cmd(4, argv[2], NULL, NULL);

    return 0;
}
