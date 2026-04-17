#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define SOCK_PATH "/tmp/engine.sock"
#define MAX_CONTAINERS 10
#define STACK_SIZE (1024 * 1024)

#define BUFFER_SIZE 10
#define LOG_SIZE 256

/* ===================== LOG BUFFER ===================== */
typedef struct {
char data[LOG_SIZE];
char filename[100];
} LogItem;

LogItem buffer[BUFFER_SIZE];
int in = 0, out = 0, count = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

int logging_active = 1;

/* ===================== CONTAINER ===================== */
typedef struct {
char name[50];
pid_t pid;
time_t start_time;
int state; // 1 running, 2 stopped
int stop_requested;
char exit_reason[50];
} Container;

Container containers[MAX_CONTAINERS];
char child_stack[STACK_SIZE];

int monitor_fd;

/* ===================== PRODUCER ===================== */
void* producer(void *arg) {
int fd = *(int*)arg;
char buf[LOG_SIZE];


while (1) {
    int n = read(fd, buf, sizeof(buf)-1);
    if (n <= 0) break;

    buf[n] = '\0';

    pthread_mutex_lock(&lock);
    while (count == BUFFER_SIZE)
        pthread_cond_wait(&not_full, &lock);

    strcpy(buffer[in].data, buf);
    strcpy(buffer[in].filename, "container.log");

    in = (in + 1) % BUFFER_SIZE;
    count++;

    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&lock);
}

close(fd);
return NULL;


}

/* ===================== CONSUMER ===================== */
void* consumer(void *arg) {
while (logging_active || count > 0) {
pthread_mutex_lock(&lock);


    while (count == 0 && logging_active)
        pthread_cond_wait(&not_empty, &lock);

    if (count == 0 && !logging_active) {
        pthread_mutex_unlock(&lock);
        break;
    }

    LogItem item = buffer[out];
    out = (out + 1) % BUFFER_SIZE;
    count--;

    pthread_cond_signal(&not_full);
    pthread_mutex_unlock(&lock);

    FILE *f = fopen(item.filename, "a");
    if (f) {
        fprintf(f, "%s", item.data);
        fclose(f);
    }
}
return NULL;


}

/* ===================== CONTAINER MAIN ===================== */
int container_main(void *arg) {
int *pipefd = (int*)arg;


dup2(pipefd[1], STDOUT_FILENO);
dup2(pipefd[1], STDERR_FILENO);
close(pipefd[0]);
close(pipefd[1]);

chroot("./rootfs");
chdir("/");
mount("proc", "/proc", "proc", 0, NULL);

execl("/bin/sh", "sh", "-c", "./memory_hog 5 200", NULL);

return 1;


}

/* ===================== SIGNALS ===================== */
void reap_children(int sig) {
while (waitpid(-1, NULL, WNOHANG) > 0);
}

void handle_exit(int sig) {
logging_active = 0;
pthread_cond_broadcast(&not_empty);
}

/* ===================== START CONTAINER ===================== */
void start_container(char *name, int foreground) {
int pipefd[2];
pipe(pipefd);


pid_t pid = clone(container_main,
                  child_stack + STACK_SIZE,
                  CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                  pipefd);

close(pipefd[1]);

pthread_t prod;
pthread_create(&prod, NULL, producer, &pipefd[0]);

int i;
for (i = 0; i < MAX_CONTAINERS; i++) {
    if (containers[i].state == 0) break;
}

containers[i].pid = pid;
strcpy(containers[i].name, name);
containers[i].start_time = time(NULL);
containers[i].state = 1;
containers[i].stop_requested = 0;
strcpy(containers[i].exit_reason, "running");

/* ===== REGISTER WITH KERNEL ===== */
struct monitor_request req;
req.pid = pid;
strcpy(req.container_id, name);
req.soft_limit_bytes = 20 * 1024 * 1024;  // 20MB
req.hard_limit_bytes = 40 * 1024 * 1024;  // 40MB

ioctl(monitor_fd, MONITOR_REGISTER, &req);

printf("[supervisor] Started %s PID %d\n", name, pid);

if (foreground) {
    int status;
    waitpid(pid, &status, 0);

    containers[i].state = 2;

    if (containers[i].stop_requested) {
        strcpy(containers[i].exit_reason, "stopped");
    }
    else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
        strcpy(containers[i].exit_reason, "hard_limit_killed");
    }
    else {
        strcpy(containers[i].exit_reason, "normal");
    }

    printf("[supervisor] Container %s exited (%s)\n",
           name, containers[i].exit_reason);
}


}

/* ===================== STOP ===================== */
void stop_container(char *name) {
for (int i = 0; i < MAX_CONTAINERS; i++) {
if (strcmp(containers[i].name, name) == 0) {
containers[i].stop_requested = 1;
kill(containers[i].pid, SIGTERM);
containers[i].state = 2;
}
}
}

/* ===================== PS ===================== */
void list_containers() {
printf("NAME\tPID\tSTATE\tREASON\n");
for (int i = 0; i < MAX_CONTAINERS; i++) {
if (containers[i].state != 0) {
printf("%s\t%d\t%s\t%s\n",
containers[i].name,
containers[i].pid,
containers[i].state == 1 ? "running" : "stopped",
containers[i].exit_reason);
}
}
}

/* ===================== LOGS ===================== */
void show_logs() {
FILE *f = fopen("container.log", "r");
if (!f) return;


char line[256];
while (fgets(line, sizeof(line), f)) {
    printf("%s", line);
}
fclose(f);


}

/* ===================== SUPERVISOR ===================== */
void run_supervisor() {
int server_fd, client_fd;
struct sockaddr_un addr;


signal(SIGCHLD, reap_children);
signal(SIGINT, handle_exit);
signal(SIGTERM, handle_exit);

pthread_t cons;
pthread_create(&cons, NULL, consumer, NULL);

monitor_fd = open("/dev/container_monitor", O_RDWR);

server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

memset(&addr, 0, sizeof(addr));
addr.sun_family = AF_UNIX;
strcpy(addr.sun_path, SOCK_PATH);

unlink(SOCK_PATH);
bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
listen(server_fd, 5);

chmod(SOCK_PATH, 0666);

printf("[supervisor] Listening on %s\n", SOCK_PATH);

while (1) {
    client_fd = accept(server_fd, NULL, NULL);

    char buf[256];
    int n = read(client_fd, buf, sizeof(buf)-1);
    buf[n] = '\0';

    printf("[supervisor] Received: %s\n", buf);

    if (strncmp(buf, "start", 5) == 0) {
        char name[50];
        sscanf(buf, "start %s", name);
        start_container(name, 0);
    }
    else if (strncmp(buf, "run", 3) == 0) {
        char name[50];
        sscanf(buf, "run %s", name);
        start_container(name, 1);
    }
    else if (strncmp(buf, "ps", 2) == 0) {
        list_containers();
    }
    else if (strncmp(buf, "logs", 4) == 0) {
        show_logs();
    }
    else if (strncmp(buf, "stop", 4) == 0) {
        char name[50];
        sscanf(buf, "stop %s", name);
        stop_container(name);
    }

    close(client_fd);
}


}

/* ===================== CLIENT ===================== */
void run_client(int argc, char **argv) {
int fd;
struct sockaddr_un addr;


fd = socket(AF_UNIX, SOCK_STREAM, 0);

memset(&addr, 0, sizeof(addr));
addr.sun_family = AF_UNIX;
strcpy(addr.sun_path, SOCK_PATH);

connect(fd, (struct sockaddr *)&addr, sizeof(addr));

char cmd[256] = "";
for (int i = 1; i < argc; i++) {
    strcat(cmd, argv[i]);
    strcat(cmd, " ");
}

write(fd, cmd, strlen(cmd));
close(fd);


}

/* ===================== MAIN ===================== */
int main(int argc, char **argv) {
if (argc < 2) {
printf("Usage:\n");
printf("  sudo ./engine supervisor\n");
printf("  sudo ./engine start <name>\n");
printf("  sudo ./engine run <name>\n");
printf("  sudo ./engine ps\n");
printf("  sudo ./engine logs\n");
printf("  sudo ./engine stop <name>\n");
return 1;
}


if (strcmp(argv[1], "supervisor") == 0) {
    run_supervisor();
} else {
    run_client(argc, argv);
}

return 0;


}

