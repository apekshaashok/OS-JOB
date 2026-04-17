#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static size_t parse_size_mb(const char *arg, size_t fallback)
{
char *end = NULL;
unsigned long value = strtoul(arg, &end, 10);


if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
    return fallback;
return (size_t)value;


}

static useconds_t parse_sleep_ms(const char *arg, useconds_t fallback)
{
char *end = NULL;
unsigned long value = strtoul(arg, &end, 10);


if (!arg || *arg == '\0' || (end && *end != '\0'))
    return fallback;
return (useconds_t)(value * 1000U);


}

/* graceful exit message when killed */
void handle_sig(int sig)
{
printf("memory_hog killed by signal %d\n", sig);
fflush(stdout);
exit(0);
}

int main(int argc, char *argv[])
{
signal(SIGTERM, handle_sig);
signal(SIGKILL, handle_sig); // (won't catch, but safe to include)
signal(SIGINT, handle_sig);


const size_t chunk_mb = (argc > 1) ? parse_size_mb(argv[1], 5) : 5;
const useconds_t sleep_us = (argc > 2) ? parse_sleep_ms(argv[2], 200U) : 200U * 1000U;

const size_t chunk_bytes = chunk_mb * 1024U * 1024U;
int count = 0;

printf("memory_hog started: chunk=%zuMB sleep=%dus\n", chunk_mb, sleep_us);
fflush(stdout);

while (1) {
    char *mem = malloc(chunk_bytes);
    if (!mem) {
        printf("malloc failed after %d allocations\n", count);
        fflush(stdout);
        break;
    }

    /* touch every page to ensure RSS increases */
    for (size_t i = 0; i < chunk_bytes; i += 4096) {
        mem[i] = 'A';
    }

    count++;

    printf("allocated=%d chunks total=%zuMB\n",
           count, (size_t)count * chunk_mb);
    fflush(stdout);

    usleep(sleep_us);
}

return 0;


}

