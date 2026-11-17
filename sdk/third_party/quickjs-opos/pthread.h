/*
 * Single-threaded stand-ins for the pthread calls QuickJS makes (written
 * for OPOS, not part of QuickJS). OPOS programs have one thread, so a
 * mutex never waits. Waiting on a condition would wait forever, so the
 * browser turns blocking off (JS_SetCanBlock) and Atomics.wait throws
 * instead of reaching pthread_cond_wait.
 */
#ifndef OPOS_QUICKJS_PTHREAD_H
#define OPOS_QUICKJS_PTHREAD_H

#include <errno.h>
#include <time.h>

typedef struct {
    int locked;
} pthread_mutex_t;
typedef struct {
    int unused;
} pthread_cond_t;
typedef struct {
    int unused;
} pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    mutex->locked = 1;
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    mutex->locked = 0;
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *cond,
                                    const pthread_condattr_t *attr) {
    (void)cond;
    (void)attr;
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond,
                                    pthread_mutex_t *mutex) {
    (void)cond;
    (void)mutex;
    return 0;
}

static inline int pthread_cond_timedwait(pthread_cond_t *cond,
                                         pthread_mutex_t *mutex,
                                         const struct timespec *time) {
    (void)cond;
    (void)mutex;
    (void)time;
    return ETIMEDOUT;
}

#endif
