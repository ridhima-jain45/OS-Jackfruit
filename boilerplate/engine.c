/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation covering all six tasks:
 *
 *   Task 1  Multi-container lifecycle with PID/UTS/mount namespace isolation
 *   Task 2  UNIX socket CLI + SIGCHLD/SIGTERM/SIGINT signal handling
 *   Task 3  Bounded-buffer producer/consumer logging pipeline
 *   Task 4  Kernel monitor registration/unregistration via ioctl
 *   Task 5  Scheduling experiment support (--nice flag)
 *   Task 6  Complete resource cleanup on shutdown
 *
 * IPC mechanisms used:
 *   Path A (logging)  : pipe per container -> bounded ring buffer -> log file
 *   Path B (control)  : UNIX domain stream socket at CONTROL_PATH
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o engine engine.c -lpthread
 */

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

/* ======================================================================
 *  COMPILE-TIME CONSTANTS
 * ====================================================================== */

#define STACK_SIZE            (1024 * 1024)
#define CONTAINER_ID_LEN      32
#define CONTROL_PATH          "/tmp/mini_runtime.sock"
#define LOG_DIR               "logs"
#define LOG_CHUNK_SIZE        4096
#define LOG_BUFFER_CAPACITY   32
#define MSG_LEN               4096      /* control response / ps table */
#define CMD_LEN               256
#define DEFAULT_SOFT_LIMIT    (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT    (64UL << 20)   /* 64 MiB */

/* ======================================================================
 *  ENUMERATIONS
 * ====================================================================== */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP,
    CMD_WAIT        /* internal: block until named container exits */
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef enum {
    TERM_NONE       = 0,
    TERM_NORMAL,        /* exited with code 0 */
    TERM_NONZERO,       /* exited with non-zero code */
    TERM_STOPPED,       /* supervisor issued stop (stop_requested flag set) */
    TERM_HARD_LIMIT,    /* SIGKILL from kernel monitor (stop_requested == 0) */
    TERM_SIGNAL         /* other signal */
} term_reason_t;

/* ======================================================================
 *  DATA STRUCTURES
 * ====================================================================== */

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;   /* set before sending stop signal */
    term_reason_t      term_reason;
    char               log_path[PATH_MAX];
    pthread_cond_t     exited_cond;      /* signalled in reap_children() */
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/* Wire format for the control channel (Path B) */
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CMD_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    int  exit_code;
    int  exit_signal;
    char term_reason[32];
    char message[MSG_LEN];
} control_response_t;

/* Passed through clone() into the child entrypoint */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CMD_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

/* Global supervisor state */
typedef struct {
    int              server_fd;
    int              monitor_fd;
    volatile int     should_stop;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Argument block for per-container producer threads */
typedef struct {
    int              pipe_fd;
    supervisor_ctx_t *ctx;
    char             id[CONTAINER_ID_LEN];
} producer_arg_t;

/* ======================================================================
 *  GLOBALS (signal state + context pointer for handlers)
 * ====================================================================== */

static volatile sig_atomic_t g_sigchld_pending = 0;
static volatile sig_atomic_t g_shutdown        = 0;
static supervisor_ctx_t     *g_ctx             = NULL;

/* Used by cmd_run to name the container when SIGINT arrives */
static char                  g_run_container_id[CONTAINER_ID_LEN];
static volatile sig_atomic_t g_run_interrupted  = 0;

/* ======================================================================
 *  FORWARD DECLARATIONS
 * ====================================================================== */

static int  bounded_buffer_push(bounded_buffer_t *, const log_item_t *);
static int  bounded_buffer_pop (bounded_buffer_t *, log_item_t *);
static void *producer_thread(void *);
static void *logging_thread (void *);
static int   child_fn(void *);
static int   register_with_monitor  (int, const char *, pid_t, unsigned long, unsigned long);
static int   unregister_from_monitor(int, const char *, pid_t);
static container_record_t *find_container(supervisor_ctx_t *, const char *);

/* ======================================================================
 *  USAGE / ARGUMENT PARSING
 * ====================================================================== */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *val,
                           unsigned long *out_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(val, &end, 10);
    if (errno || end == val || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, val);
        return -1;
    }
    if (mib > ULONG_MAX >> 20) {
        fprintf(stderr, "Value for %s too large: %s\n", flag, val);
        return -1;
    }
    *out_bytes = mib << 20;
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                 int argc, char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes))
                return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes))
                return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end = NULL;
            long nv;
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice (must be -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_str(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static const char *term_str(term_reason_t r)
{
    switch (r) {
    case TERM_NONE:       return "none";
    case TERM_NORMAL:     return "normal_exit";
    case TERM_NONZERO:    return "nonzero_exit";
    case TERM_STOPPED:    return "stopped";
    case TERM_HARD_LIMIT: return "hard_limit_killed";
    case TERM_SIGNAL:     return "signal";
    default:              return "unknown";
    }
}

static int container_is_live(container_state_t s)
{
    return s == CONTAINER_STARTING || s == CONTAINER_RUNNING;
}

/* ======================================================================
 *  BOUNDED BUFFER
 *
 *  Synchronisation design:
 *    - pthread_mutex_t mutex     : serialises all reads/writes of head/tail/count
 *    - pthread_cond_t  not_full  : producers wait here when count == capacity
 *    - pthread_cond_t  not_empty : consumer waits here when count == 0
 *    - int shutting_down         : set by begin_shutdown(); broadcasts both conds
 *
 *  Without synchronisation:
 *    - Two concurrent producers could compute the same tail index,
 *      both write there, and one entry would be silently overwritten.
 *    - A consumer could observe count > 0 but read a partially written item
 *      (torn write) because no ordering guarantee exists on the data fields.
 *    - Missed wake-ups: a consumer decrements count to 0 after a producer
 *      checked and found count == capacity; the producer sleeps forever.
 *
 *  Shutdown / drain guarantee:
 *    Producers stop pushing after shutting_down is set.
 *    Consumer continues popping until count == 0 *and* shutting_down == 1,
 *    then returns -1.  This ensures every byte written before shutdown is
 *    flushed to disk.
 * ====================================================================== */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    if ((rc = pthread_mutex_init(&buf->mutex, NULL))) return rc;
    if ((rc = pthread_cond_init(&buf->not_empty, NULL))) {
        pthread_mutex_destroy(&buf->mutex); return rc;
    }
    if ((rc = pthread_cond_init(&buf->not_full, NULL))) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex); return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/* Producer: block if full; return -1 on shutdown (caller should stop) */
static int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* Consumer: block if empty; return -1 when shutdown AND buffer drained */
static int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0) {   /* shutdown + empty: truly done */
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ======================================================================
 *  PRODUCER THREAD  (one per container, detached)
 *
 *  Reads raw bytes from the container's stdout/stderr pipe and pushes
 *  LOG_CHUNK_SIZE-aligned chunks into the shared bounded buffer.
 *  Exits when read() returns 0 (pipe closed after container exits).
 *  Frees its own argument struct before returning.
 * ====================================================================== */

static void *producer_thread(void *arg)
{
    producer_arg_t *p = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(p->pipe_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, p->id, CONTAINER_ID_LEN - 1);
        memcpy(item.data, buf, (size_t)n);
        item.length = (size_t)n;
        /* If shutdown is in progress push returns -1; we just stop. */
        if (bounded_buffer_push(&p->ctx->log_buffer, &item) < 0)
            break;
    }

    close(p->pipe_fd);
    free(p);
    return NULL;
}

/* ======================================================================
 *  LOGGING (CONSUMER) THREAD  (one per supervisor, joinable)
 *
 *  Drains the bounded buffer and appends each chunk to the appropriate
 *  per-container log file.  Continues draining until both shutting_down
 *  is set and the buffer is empty, guaranteeing no log lines are lost.
 * ====================================================================== */

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            size_t written = 0;
            while (written < item.length) {
                ssize_t w = write(fd, item.data + written,
                                  item.length - written);
                if (w <= 0) break;
                written += (size_t)w;
            }
            close(fd);
        }
    }
    return NULL;
}

/* ======================================================================
 *  CHILD ENTRYPOINT  (executes inside clone())
 *
 *  Isolation steps:
 *    1. UTS namespace:   sethostname() to container id
 *    2. Mount namespace: chroot() to container-specific rootfs
 *    3. PID namespace:   mount /proc so ps/top work
 *    4. Logging:         dup2 stdout+stderr onto log pipe write-end
 *    5. Scheduling:      nice() if requested
 *    6. Execute:         execl /bin/sh -c <command>
 * ====================================================================== */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* 1. UTS: set hostname */
    sethostname(cfg->id, strlen(cfg->id));

    /* 2. Mount: pivot to container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* 3. PID: mount proc in our own mount namespace */
    mount("proc", "/proc", "proc", 0, NULL);

    /* 4. Logging: redirect stdout + stderr to pipe write-end */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* 5. Scheduling */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* 6. Exec */
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("execl");
    return 127;
}

/* ======================================================================
 *  KERNEL MONITOR IOCTL WRAPPERS
 * ====================================================================== */

static int register_with_monitor(int monitor_fd, const char *container_id,
                                  pid_t host_pid,
                                  unsigned long soft_bytes,
                                  unsigned long hard_bytes)
{
    if (monitor_fd < 0) return 0;   /* module not loaded: skip */

    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_bytes;
    req.hard_limit_bytes = hard_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl MONITOR_REGISTER");
        return -1;
    }
    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id,
                                    pid_t host_pid)
{
    if (monitor_fd < 0) return 0;

    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    ioctl(monitor_fd, MONITOR_UNREGISTER, &req);  /* -ENOENT is acceptable */
    return 0;
}

/* ======================================================================
 *  METADATA HELPERS  (caller must hold metadata_lock)
 * ====================================================================== */

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    for (container_record_t *c = ctx->containers; c; c = c->next)
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
    return NULL;
}

/* ======================================================================
 *  SIGNAL HANDLERS + CHILD REAPING
 * ====================================================================== */

static void sig_chld_handler(int signo) { (void)signo; g_sigchld_pending = 1; }
static void sig_term_handler(int signo) { (void)signo; g_shutdown = 1; }
static void run_sig_handler (int signo) { (void)signo; g_run_interrupted = 1; }

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sig_chld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sig_term_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
}

/*
 * reap_children -- called from the main event loop when g_sigchld_pending
 * is set.  Calls waitpid(-1, WNOHANG) in a loop to drain all exited
 * children; updates each container's metadata and wakes CMD_WAIT clients.
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int   wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);

        for (container_record_t *c = ctx->containers; c; c = c->next) {
            if (c->host_pid != pid) continue;

            if (WIFEXITED(wstatus)) {
                c->exit_code   = WEXITSTATUS(wstatus);
                c->exit_signal = 0;
                if (c->stop_requested) {
                    c->state = CONTAINER_STOPPED; c->term_reason = TERM_STOPPED;
                } else {
                    c->state = CONTAINER_EXITED;
                    c->term_reason = c->exit_code ? TERM_NONZERO : TERM_NORMAL;
                }
            } else if (WIFSIGNALED(wstatus)) {
                c->exit_signal = WTERMSIG(wstatus);
                c->exit_code   = 0;
                if (c->stop_requested) {
                    c->state = CONTAINER_STOPPED; c->term_reason = TERM_STOPPED;
                } else if (c->exit_signal == SIGKILL) {
                    c->state = CONTAINER_KILLED; c->term_reason = TERM_HARD_LIMIT;
                } else {
                    c->state = CONTAINER_KILLED; c->term_reason = TERM_SIGNAL;
                }
            }

            /* Wake CMD_WAIT clients blocking on this container */
            pthread_cond_broadcast(&c->exited_cond);

            /* Unregister from kernel monitor */
            unregister_from_monitor(ctx->monitor_fd, c->id, pid);

            printf("[supervisor] %s pid=%d -> state=%s reason=%s\n",
                   c->id, pid, state_str(c->state), term_str(c->term_reason));
            fflush(stdout);
            break;
        }

        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ======================================================================
 *  SUPERVISOR COMMAND HANDLERS
 * ====================================================================== */

static void handle_start(supervisor_ctx_t *ctx,
                          const control_request_t *req,
                          control_response_t *resp)
{
    /* Reject duplicate container IDs */
    pthread_mutex_lock(&ctx->metadata_lock);
    int dup = find_container(ctx, req->container_id) != NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (dup) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: container '%s' already exists", req->container_id);
        return;
    }

    /* Create logging pipe: container stdout/stderr -> producer thread */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: pipe: %s", strerror(errno));
        return;
    }

    /* child_config lives on the heap; the child reads it before exec */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "ERROR: calloc cfg");
        return;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CMD_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Clone stack: allocated in parent, used by child until exec */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg); close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "ERROR: malloc stack");
        return;
    }

    mkdir(LOG_DIR, 0755);

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    /* Parent no longer needs write-end of pipe */
    close(pipefd[1]);

    if (pid < 0) {
        free(stack); free(cfg); close(pipefd[0]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: clone: %s", strerror(errno));
        return;
    }

    /* Stack memory: safe to free now — child has its own address space after
     * clone().  cfg is accessed by child before exec; on Linux clone() with
     * CLONE_VM is not set (default), so child and parent have separate COW
     * address spaces.  Freeing here is safe because child runs concurrently
     * and already has the pointer; but to be 100% safe we keep cfg alive
     * until exec replaces the address space.  We leak it intentionally for
     * the child's brief lifetime (the exec path is fast). */
    free(stack);

    /* Build metadata record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        kill(pid, SIGKILL); close(pipefd[0]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "ERROR: calloc record");
        return;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
    pthread_cond_init(&rec->exited_cond, NULL);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel memory monitor */
    register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                           req->soft_limit_bytes, req->hard_limit_bytes);

    /* Spawn detached producer thread to forward container output */
    producer_arg_t *parg = calloc(1, sizeof(*parg));
    if (parg) {
        parg->pipe_fd = pipefd[0];
        parg->ctx     = ctx;
        strncpy(parg->id, req->container_id, CONTAINER_ID_LEN - 1);

        pthread_t prod;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&prod, &attr, producer_thread, parg) != 0) {
            free(parg);
            close(pipefd[0]);
        }
        pthread_attr_destroy(&attr);
    } else {
        close(pipefd[0]);
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "OK: started '%s' pid=%d", req->container_id, pid);
    printf("[supervisor] %s\n", resp->message);
    fflush(stdout);
}

/*
 * handle_wait -- blocks the control connection until the named container
 * is no longer live, then returns its final metadata.  This is the mechanism
 * that gives `engine run` its blocking semantics without polling.
 */
static void handle_wait(supervisor_ctx_t *ctx,
                          const control_request_t *req,
                          control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);

    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: container '%s' not found", req->container_id);
        return;
    }

    /* Block on per-container condition variable until reap_children() signals */
    while (container_is_live(c->state))
        pthread_cond_wait(&c->exited_cond, &ctx->metadata_lock);

    resp->status      = 0;
    resp->exit_code   = c->exit_code;
    resp->exit_signal = c->exit_signal;
    strncpy(resp->term_reason, term_str(c->term_reason),
            sizeof(resp->term_reason) - 1);
    snprintf(resp->message, sizeof(resp->message),
             "OK: '%s' state=%s reason=%s exit_code=%d",
             c->id, state_str(c->state), term_str(c->term_reason), c->exit_code);

    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char buf[MSG_LEN];
    int  off = 0;
    int  rem = (int)sizeof(buf);

    off += snprintf(buf, sizeof(buf),
        "%-16s %-7s %-10s %-6s %-9s %-9s %-20s\n",
        "ID", "PID", "STATE", "EXIT", "SOFT(MiB)", "HARD(MiB)", "REASON");
    rem -= off;

    pthread_mutex_lock(&ctx->metadata_lock);
    int any = 0;
    for (container_record_t *c = ctx->containers; c && rem > 1; c = c->next) {
        int n = snprintf(buf + off, (size_t)rem,
            "%-16s %-7d %-10s %-6d %-9lu %-9lu %-20s\n",
            c->id, c->host_pid, state_str(c->state), c->exit_code,
            c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20,
            term_str(c->term_reason));
        off += n; rem -= n;
        any = 1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!any)
        snprintf(buf + off, (size_t)rem, "(no containers)\n");

    resp->status = 0;
    strncpy(resp->message, buf, sizeof(resp->message) - 1);
    resp->message[sizeof(resp->message) - 1] = '\0';
}

static void handle_logs(supervisor_ctx_t *ctx,
                         const control_request_t *req,
                         control_response_t *resp)
{
    char log_path[PATH_MAX];
    log_path[0] = '\0';

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (c) strncpy(log_path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: container '%s' not found", req->container_id);
        return;
    }

    resp->status = 0;
    /* Prefix LOG: so client knows to read the file at this path */
    snprintf(resp->message, sizeof(resp->message), "LOG:%s", log_path);
}

static void handle_stop(supervisor_ctx_t *ctx,
                         const control_request_t *req,
                         control_response_t *resp)
{
    pid_t pid = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (c && container_is_live(c->state)) {
        c->stop_requested = 1;
        pid = c->host_pid;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!pid) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: '%s' not found or not running", req->container_id);
        return;
    }

    /* Graceful: SIGTERM then wait up to 3 s, then SIGKILL */
    kill(pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        pthread_mutex_lock(&ctx->metadata_lock);
        c = find_container(ctx, req->container_id);
        int live = c && container_is_live(c->state);
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (!live) goto done;
    }
    kill(pid, SIGKILL);

done:
    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "OK: stop signalled for '%s'", req->container_id);
}

/* ======================================================================
 *  SUPERVISOR MAIN LOOP
 * ====================================================================== */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs;  /* informational only; per-container path comes via CLI */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    if ((rc = pthread_mutex_init(&ctx.metadata_lock, NULL))) {
        errno = rc; perror("pthread_mutex_init"); return 1;
    }
    if ((rc = bounded_buffer_init(&ctx.log_buffer))) {
        errno = rc; perror("bounded_buffer_init"); return 1;
    }

    /* Optional kernel monitor */
    ctx.monitor_fd = open("/dev/" DEVICE_NAME, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] WARNING: /dev/%s not found -- memory monitoring disabled\n",
                DEVICE_NAME);

    /* UNIX domain control socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    chmod(CONTROL_PATH, 0777);
    if (listen(ctx.server_fd, 32) < 0) { perror("listen"); return 1; }

    /* Non-blocking so select() drives the event loop */
    {
        int fl = fcntl(ctx.server_fd, F_GETFL, 0);
        fcntl(ctx.server_fd, F_SETFL, fl | O_NONBLOCK);
    }

    install_signals();

    if ((rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx))) {
        errno = rc; perror("pthread_create logger"); return 1;
    }

    printf("[supervisor] ready  socket=%s  pid=%d\n", CONTROL_PATH, getpid());
    fflush(stdout);

    /* ---- event loop ---- */
    while (!g_shutdown) {

        if (g_sigchld_pending) {
            g_sigchld_pending = 0;
            reap_children(&ctx);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200 ms */

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (sel == 0) continue;

        int client = accept(ctx.server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            perror("accept"); break;
        }

        struct timeval rtv = { .tv_sec = 60, .tv_usec = 0 }; /* long for CMD_WAIT */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

        control_request_t  req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        ssize_t nr = read(client, &req, sizeof(req));
        if (nr != (ssize_t)sizeof(req)) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "ERROR: malformed request");
            write(client, &resp, sizeof(resp));
            close(client);
            continue;
        }

        switch (req.kind) {
        case CMD_START: handle_start(&ctx, &req, &resp); break;
        case CMD_RUN:   handle_start(&ctx, &req, &resp); break;
        case CMD_WAIT:  handle_wait (&ctx, &req, &resp); break;
        case CMD_PS:    handle_ps   (&ctx,       &resp); break;
        case CMD_LOGS:  handle_logs (&ctx, &req, &resp); break;
        case CMD_STOP:  handle_stop (&ctx, &req, &resp); break;
        default:
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: unknown command %d", req.kind);
        }

        write(client, &resp, sizeof(resp));
        close(client);
    }

    /* ---- orderly shutdown ---- */
    printf("[supervisor] shutting down...\n");
    fflush(stdout);

    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next)
        if (container_is_live(c->state)) { c->stop_requested = 1; kill(c->host_pid, SIGTERM); }
    pthread_mutex_unlock(&ctx.metadata_lock);

    usleep(500000);

    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next)
        if (container_is_live(c->state)) kill(c->host_pid, SIGKILL);
    pthread_mutex_unlock(&ctx.metadata_lock);

    reap_children(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            container_record_t *nxt = c->next;
            pthread_cond_destroy(&c->exited_cond);
            free(c);
            c = nxt;
        }
        ctx.containers = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    printf("[supervisor] clean exit.\n");
    return 0;
}

/* ======================================================================
 *  CLIENT HELPERS
 * ====================================================================== */

static int send_control_request(const control_request_t *req,
                                 control_response_t *resp_out)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect failed -- is the supervisor running?\n");
        close(sock); return -1;
    }

    /* Fully write request */
    const char *wp = (const char *)req;
    size_t wrem = sizeof(*req);
    while (wrem > 0) {
        ssize_t w = write(sock, wp, wrem);
        if (w <= 0) { perror("write req"); close(sock); return -1; }
        wp += w; wrem -= (size_t)w;
    }

    /* Fully read response */
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    char *rp = (char *)&resp;
    size_t rrem = sizeof(resp);
    while (rrem > 0) {
        ssize_t r = read(sock, rp, rrem);
        if (r <= 0) {
            fprintf(stderr, "ERROR: truncated supervisor response\n");
            close(sock); return -1;
        }
        rp += r; rrem -= (size_t)r;
    }
    close(sock);

    if (resp_out) *resp_out = resp;
    if (resp.status != 0) { fprintf(stderr, "%s\n", resp.message); return resp.status; }
    return 0;
}

/* ======================================================================
 *  CLI COMMAND FUNCTIONS (client side)
 * ====================================================================== */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CMD_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5)) return 1;

    control_response_t resp;
    int rc = send_control_request(&req, &resp);
    if (rc == 0) printf("%s\n", resp.message);
    return rc;
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CMD_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5)) return 1;

    /* Step 1: ask supervisor to launch the container */
    control_response_t start_resp;
    int rc = send_control_request(&req, &start_resp);
    if (rc) return rc;
    printf("%s\n", start_resp.message);

    /* Step 2: install signal forwarding for Ctrl-C */
    strncpy(g_run_container_id, req.container_id, CONTAINER_ID_LEN - 1);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_sig_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Step 3: send CMD_WAIT -- supervisor blocks until container exits */
    printf("[run] blocking until '%s' finishes...\n", req.container_id);
    fflush(stdout);

    control_request_t wait_req;
    memset(&wait_req, 0, sizeof(wait_req));
    wait_req.kind = CMD_WAIT;
    strncpy(wait_req.container_id, req.container_id, CONTAINER_ID_LEN - 1);

    control_response_t wait_resp;
    memset(&wait_resp, 0, sizeof(wait_resp));
    int wait_rc = send_control_request(&wait_req, &wait_resp);

    /* If interrupted: forward stop to supervisor */
    if (g_run_interrupted) {
        control_request_t stop_req;
        memset(&stop_req, 0, sizeof(stop_req));
        stop_req.kind = CMD_STOP;
        strncpy(stop_req.container_id, req.container_id, CONTAINER_ID_LEN - 1);
        send_control_request(&stop_req, NULL);
        fprintf(stderr, "[run] interrupted -- stop forwarded for '%s'\n",
                req.container_id);
        return 130;
    }

    if (wait_rc == 0) {
        printf("%s\n", wait_resp.message);
        /* Propagate container exit code per spec */
        if (strcmp(wait_resp.term_reason, "normal_exit")  == 0 ||
            strcmp(wait_resp.term_reason, "nonzero_exit") == 0)
            return wait_resp.exit_code;
        if (strcmp(wait_resp.term_reason, "signal")       == 0 ||
            strcmp(wait_resp.term_reason, "hard_limit_killed") == 0)
            return 128 + (wait_resp.exit_signal ? wait_resp.exit_signal : 9);
    }
    return wait_rc;
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    control_response_t resp;
    int rc = send_control_request(&req, &resp);
    if (rc == 0) printf("%s", resp.message);
    return rc;
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);

    control_response_t resp;
    int rc = send_control_request(&req, &resp);
    if (rc) return rc;

    /* Response: "LOG:/absolute/path/to/file" */
    char *log_path = strstr(resp.message, "LOG:");
    if (!log_path) { printf("%s\n", resp.message); return 0; }
    log_path += 4;

    FILE *f = fopen(log_path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open log '%s': %s\n",
                log_path, strerror(errno));
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f))
        fputs(line, stdout);
    fclose(f);
    return 0;
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);

    control_response_t resp;
    int rc = send_control_request(&req, &resp);
    if (rc == 0) printf("%s\n", resp.message);
    return rc;
}

/* ======================================================================
 *  MAIN
 * ====================================================================== */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run  (argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs (argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop (argc, argv);

    usage(argv[0]);
    return 1;
}
