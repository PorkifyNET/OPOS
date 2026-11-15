#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_value;

static void *worker(void *argument) {
    static const struct timespec delay = {.tv_nsec = 20000000};
    if (pthread_mutex_lock(&mutex) != 0) {
        return 0;
    }
    shared_value++;
    nanosleep(&delay, 0);
    shared_value++;
    if (pthread_mutex_unlock(&mutex) != 0) {
        return 0;
    }
    return argument;
}

/* Scheduling calls: one CPU, SCHED_OTHER and nice 0 for every thread;
   real-time policies and a lower nice value are refused. */
static int scheduling_works(void) {
    struct sched_param param = {.sched_priority = 7};
    int policy = -1;
    cpu_set_t cpus;
    if (sched_yield() != 0
        || pthread_getschedparam(pthread_self(), &policy, &param) != 0
        || policy != SCHED_OTHER || param.sched_priority != 0
        || pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) != 0
        || pthread_setschedprio(pthread_self(), 0) != 0
        || sched_get_priority_max(SCHED_FIFO) != 99
        || sched_get_priority_min(SCHED_RR) != 1
        || sched_get_priority_max(SCHED_OTHER) != 0) {
        return 0;
    }
    param.sched_priority = 10;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != EPERM) {
        return 0;
    }
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0
        || setpriority(PRIO_PROCESS, 0, 5) != 0
        || setpriority(PRIO_PROCESS, 0, -5) != -1 || errno != EACCES) {
        return 0;
    }
    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0
        || CPU_COUNT(&cpus) != 1 || !CPU_ISSET(0, &cpus)
        || sched_setaffinity(0, sizeof(cpus), &cpus) != 0
        || sysconf(_SC_NPROCESSORS_ONLN) != 1) {
        return 0;
    }
    return 1;
}

int main(void) {
    static const char passed[] = "OPOS: real musl pthread passed\n";
    static char signal_test[] = "/tests/musl-signal-test";
    static char *signal_arguments[] = {signal_test, 0};
    static char *signal_environment[] = {0};
    int values[2] = {21, 42};
    pthread_t threads[2];
    void *results[2] = {0};
    if (pthread_create(&threads[0], 0, worker, &values[0]) != 0
        || pthread_create(&threads[1], 0, worker, &values[1]) != 0
        || pthread_join(threads[0], &results[0]) != 0
        || pthread_join(threads[1], &results[1]) != 0
        || results[0] != &values[0] || results[1] != &values[1]
        || shared_value != 4) {
        return 1;
    }
    if (!scheduling_works()) {
        return 3;
    }
    if (write(1, passed, sizeof(passed) - 1) != sizeof(passed) - 1) {
        return 1;
    }
    execve(signal_test, signal_arguments, signal_environment);
    return 2;
}
