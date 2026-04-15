/* 
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef struct {
    int fd;
    char log_path[PATH_MAX];
} reader_args_t;

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int pipefd[2];
} container_args_t;

typedef struct {
    int server_fd;
    int should_stop;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *global_ctx = NULL;

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static void handle_sigchld(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("Container %d exited\n", pid);

        pthread_mutex_lock(&global_ctx->metadata_lock);

        container_record_t *c = global_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status))
                    c->state = CONTAINER_EXITED;
                else if (WIFSIGNALED(status))
                    c->state = CONTAINER_KILLED;
                break;
            }
            c = c->next;
        }

        pthread_mutex_unlock(&global_ctx->metadata_lock);
    }
}

static int child_fn(void *arg)
{
    container_args_t *args = (container_args_t *)arg;

    dup2(args->pipefd[1], STDOUT_FILENO);
    dup2(args->pipefd[1], STDERR_FILENO);

    close(args->pipefd[0]);
    close(args->pipefd[1]);

    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    if (chroot(args->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }

    chdir("/");
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    execl(args->command, args->command, NULL);

    perror("execl failed");
    exit(1);
}

static pid_t start_container(supervisor_ctx_t *ctx,
                             const char *id,
                             const char *rootfs,
                             const char *command,
                             size_t soft,
                             size_t hard)
{
    char *stack = malloc(STACK_SIZE);
    container_args_t *args = malloc(sizeof(container_args_t));

    int pipefd[2];
    pipe(pipefd);

    strncpy(args->rootfs, rootfs, PATH_MAX - 1);
    strncpy(args->command, command, CHILD_COMMAND_LEN - 1);

    args->pipefd[0] = pipefd[0];
    args->pipefd[1] = pipefd[1];

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      args);

    close(pipefd[1]);

    printf("Started container %s PID %d\n", id, pid);

    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req;
        memset(&req, 0, sizeof(req));

        req.pid = pid;
        req.soft_limit_bytes = soft;
        req.hard_limit_bytes = hard;

        strncpy(req.container_id, id, sizeof(req.container_id) - 1);
        req.container_id[sizeof(req.container_id) - 1] = '\0';

        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }

    container_record_t *rec = malloc(sizeof(container_record_t));
    memset(rec, 0, sizeof(*rec));

    strncpy(rec->id, id, sizeof(rec->id) - 1);
    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    return pid;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    global_ctx = &ctx;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    signal(SIGCHLD, handle_sigchld);

    mkfifo("/tmp/container_fifo", 0666);

    printf("Supervisor started with base rootfs: %s\n", rootfs);

    while (1) {
        int fd = open("/tmp/container_fifo", O_RDONLY);

        if (fd >= 0) {
            control_request_t req;
            read(fd, &req, sizeof(req));

            if (req.kind == CMD_START) {
                start_container(&ctx,
                                req.container_id,
                                req.rootfs,
                                req.command,
                                req.soft_limit_bytes,
                                req.hard_limit_bytes);
            }

            close(fd);
        }
    }
}

static int send_control_request(const control_request_t *req)
{
    int fd = open("/tmp/container_fifo", O_WRONLY);
    write(fd, req, sizeof(*req));
    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor(argv[2]);

    if (strcmp(argv[1], "start") == 0) {
        control_request_t req;
        memset(&req, 0, sizeof(req));

        req.kind = CMD_START;
        strcpy(req.container_id, argv[2]);
        strcpy(req.rootfs, argv[3]);
        strcpy(req.command, argv[4]);

        req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
        req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

        return send_control_request(&req);
    }

    return 0;
}