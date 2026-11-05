#include <pthread.h>
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

int main(void) {
    static const char passed[] = "OPOS: real musl pthread passed\n";
    static char signal_test[] = "/bin/musl-signal-test";
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
    if (write(1, passed, sizeof(passed) - 1) != sizeof(passed) - 1) {
        return 1;
    }
    execve(signal_test, signal_arguments, signal_environment);
    return 2;
}
