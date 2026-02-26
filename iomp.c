#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>
#include <sys/types.h>
#if defined(__BSD__)
#include <sys/sysctl.h>
#endif
#include "iomp_queue.h"
#include "iomp.h"

struct iomp_thread {
    TAILQ_ENTRY(iomp_thread) entries;
    iomp_t iomp;
    pthread_t thread;
    iomp_queue_t queue;
};
typedef struct iomp_thread* iomp_thread_t;

struct iomp_aiojb {
    STAILQ_ENTRY(iomp_aiojb) entries;
    struct iomp_aio* aio;
    void (*execute)(struct iomp_aiojb* job, iomp_thread_t thread);
};
typedef struct iomp_aiojb* iomp_aiojb_t;

struct iomp_core {
    pthread_mutex_t lock;
    pthread_cond_t quit;
    STAILQ_HEAD(, iomp_aiojb) jobs;
    struct iomp_aiojb stop;
    TAILQ_HEAD(, iomp_thread) actived;
    TAILQ_HEAD(, iomp_thread) blocked;
    TAILQ_HEAD(, iomp_thread) zombies;
};

static int get_ncpu();

static iomp_thread_t iomp_thread_new(iomp_t iomp, int nevents);
static void iomp_thread_drop(iomp_thread_t t);
static void* iomp_thread_run(void* arg);

static void do_post(iomp_t iomp, iomp_aiojb_t job);

static void do_stop(iomp_aiojb_t job, iomp_thread_t thread);
static void do_read(iomp_aiojb_t job, iomp_thread_t thread);
static void do_write(iomp_aiojb_t job, iomp_thread_t thread);

#define DUMP_THREADS(iomp) \
    do { \
        IOMP_LOG(DEBUG, ">>>>>>>>>"); \
        iomp_thread_t t = NULL; \
        int i = 0; \
        TAILQ_FOREACH(t, &iomp->actived, entries) { \
            IOMP_LOG(DEBUG, "thread %03d actived", i++); \
        } \
        TAILQ_FOREACH(t, &iomp->blocked, entries) { \
            IOMP_LOG(DEBUG, "thread %03d blocked", i++); \
        } \
        TAILQ_FOREACH(t, &iomp->zombies, entries) { \
            IOMP_LOG(DEBUG, "zombie thread %03d", i++); \
        } \
        IOMP_LOG(DEBUG, ">>>>>>>>>"); \
    } while (0)

iomp_t iomp_new(int nthreads) {
    if (nthreads <= 0) {
        nthreads = get_ncpu();
    }
    if (nthreads <= 0) {
        errno = EINVAL;
        return NULL;
    }
    iomp_t iomp = (iomp_t)malloc(sizeof(*iomp));
    if (!iomp) {
        IOMP_LOG(ERROR, "malloc fail: %s", strerror(errno));
        return NULL;
    }
    STAILQ_INIT(&iomp->jobs);
    TAILQ_INIT(&iomp->actived);
    TAILQ_INIT(&iomp->blocked);
    TAILQ_INIT(&iomp->zombies);
    iomp->stop.execute = do_stop;
    int rv = pthread_mutex_init(&iomp->lock, NULL);
    if (rv != 0) {
        IOMP_LOG(ERROR, "pthread_mutex_init fail: %s", strerror(rv));
        free(iomp);
        return NULL;
    }
    rv = pthread_cond_init(&iomp->quit, NULL);
    if (rv != 0) {
        IOMP_LOG(ERROR, "pthread_cond_init fail: %s", strerror(rv));
        pthread_mutex_destroy(&iomp->lock);
        free(iomp);
        return NULL;
    }
    pthread_mutex_lock(&iomp->lock);
    for (int i = 0; i < nthreads; i++) {
        iomp_thread_t t = iomp_thread_new(iomp, IOMP_EVENT_LIMIT);
        if (t) {
            TAILQ_INSERT_TAIL(&iomp->actived, t, entries);
        }
    }
    pthread_mutex_unlock(&iomp->lock);
    return iomp;
}

void iomp_drop(iomp_t iomp) {
    if (!iomp) {
        return;
    }
    do_post(iomp, &iomp->stop);
    pthread_mutex_lock(&iomp->lock);
    pthread_cond_wait(&iomp->quit, &iomp->lock);
    while (!TAILQ_EMPTY(&iomp->zombies)) {
        iomp_thread_t t = TAILQ_FIRST(&iomp->zombies);
        TAILQ_REMOVE(&iomp->zombies, t, entries);
        iomp_thread_drop(t);
    }
    pthread_mutex_unlock(&iomp->lock);
    pthread_cond_destroy(&iomp->quit);
    pthread_mutex_destroy(&iomp->lock);
    free(iomp);
}

void iomp_read(iomp_t iomp, iomp_aio_t aio) {
    if (!aio || !aio->complete) {
        IOMP_LOG(ERROR, "invalid argument");
        return;
    }
    if (!iomp || !aio->buf) {
        aio->complete(aio, EINVAL);
        return;
    }
    iomp_aiojb_t job = (iomp_aiojb_t)malloc(sizeof(*job));
    if (!job) {
        aio->complete(aio, errno);
        return;
    }
    job->aio = aio;
    aio->offset = 0;
    job->execute = do_read;
    do_post(iomp, job);
}

void iomp_write(iomp_t iomp, iomp_aio_t aio) {
    if (!aio || !aio->complete) {
        IOMP_LOG(ERROR, "invalid argument");
        return;
    }
    if (!iomp || !aio->buf) {
        aio->complete(aio, EINVAL);
        return;
    }
    iomp_aiojb_t job = (iomp_aiojb_t)malloc(sizeof(*job));
    if (!job) {
        aio->complete(aio, errno);
        return;
    }
    job->aio = aio;
    aio->offset = 0;
    job->execute = do_write;
    do_post(iomp, job);
}

void iomp_accept(iomp_t iomp, iomp_aio_t aio) {
    if (!aio || !aio->complete) {
        IOMP_LOG(ERROR, "invalid argument");
        return;
    }
    if (!iomp || aio->buf) {
        aio->complete(aio, EINVAL);
        return;
    }
    pthread_mutex_lock(&iomp->lock);
    iomp_thread_t t = NULL;
    TAILQ_FOREACH(t, &iomp->actived, entries) {
        if (iomp_queue_accept(t->queue, aio) != 0) {
            int error = errno;
            pthread_mutex_unlock(&iomp->lock);
            aio->complete(aio, error);
            return;
        }
    }
    TAILQ_FOREACH(t, &iomp->blocked, entries) {
        if (iomp_queue_accept(t->queue, aio) != 0) {
            int error = errno;
            pthread_mutex_unlock(&iomp->lock);
            aio->complete(aio, error);
            return;
        }
    }
    pthread_mutex_unlock(&iomp->lock);
}

iomp_thread_t iomp_thread_new(iomp_t iomp, int nevents) {
    iomp_thread_t t = (iomp_thread_t)malloc(sizeof(*t));
    if (!t) {
        IOMP_LOG(ERROR, "malloc fail: %s", strerror(errno));
        return NULL;
    }
    t->iomp = iomp;
    t->queue = iomp_queue_new(nevents);
    if (!t->queue) {
        IOMP_LOG(ERROR, "iomp_queue_new fail");
        free(t);
        return NULL;
    }
    int rv = pthread_create(&t->thread, NULL, iomp_thread_run, t);
    if (rv == -1) {
        IOMP_LOG(ERROR, "pthread_create fail: %s", strerror(errno));
        iomp_queue_drop(t->queue);
        free(t);
        return NULL;
    }
    return t;
}

void iomp_thread_drop(iomp_thread_t t) {
    if (!t) {
        return;
    }
    pthread_join(t->thread, NULL);
    iomp_queue_drop(t->queue);
    free(t);
}

void* iomp_thread_run(void* arg) {
    iomp_thread_t t = (iomp_thread_t)arg;
    iomp_t iomp = t->iomp;
    int stop = 0;
    pthread_mutex_lock(&iomp->lock);
    while (!stop) {
        while (STAILQ_EMPTY(&iomp->jobs)) {
            TAILQ_REMOVE(&iomp->actived, t, entries);
            TAILQ_INSERT_TAIL(&iomp->blocked, t, entries);
            pthread_mutex_unlock(&iomp->lock);
            iomp_queue_run(t->queue, -1);
            pthread_mutex_lock(&iomp->lock);
            TAILQ_REMOVE(&iomp->blocked, t, entries);
            TAILQ_INSERT_TAIL(&iomp->actived, t, entries);
        }
        iomp_aiojb_t job = STAILQ_FIRST(&iomp->jobs);
        STAILQ_REMOVE_HEAD(&iomp->jobs, entries);
        pthread_mutex_unlock(&iomp->lock);
        job->execute(job, t);
        if (job == &iomp->stop) {
            stop = 1;
        }
        pthread_mutex_lock(&iomp->lock);
    }
    pthread_mutex_unlock(&iomp->lock);
    return NULL;
}

void do_post(iomp_t iomp, iomp_aiojb_t job) {
    pthread_mutex_lock(&iomp->lock);
    STAILQ_INSERT_TAIL(&iomp->jobs, job, entries);
    if (TAILQ_EMPTY(&iomp->actived)) {
        iomp_thread_t t = TAILQ_FIRST(&iomp->blocked);
        iomp_queue_interrupt(t->queue);
    }
    pthread_mutex_unlock(&iomp->lock);
}

void do_stop(iomp_aiojb_t job, iomp_thread_t thread) {
    iomp_t iomp = thread->iomp;
    pthread_mutex_lock(&iomp->lock);
    TAILQ_REMOVE(&thread->iomp->actived, thread, entries);
    TAILQ_INSERT_TAIL(&thread->iomp->zombies, thread, entries);
    if (TAILQ_EMPTY(&iomp->actived)) {
        iomp_thread_t t = TAILQ_FIRST(&iomp->blocked);
        if (t) {
            STAILQ_INSERT_TAIL(&iomp->jobs, job, entries);
            iomp_queue_interrupt(t->queue);
        } else {
            while (!STAILQ_EMPTY(&iomp->jobs)) {
                iomp_aiojb_t job = STAILQ_FIRST(&iomp->jobs);
                STAILQ_REMOVE_HEAD(&iomp->jobs, entries);
                job->aio->complete(job->aio, -1);
                free(job);
            }
            pthread_cond_signal(&iomp->quit);
        }
    } else {
        STAILQ_INSERT_TAIL(&iomp->jobs, job, entries);
    }
    pthread_mutex_unlock(&iomp->lock);
}

void do_read(iomp_aiojb_t job, iomp_thread_t thread) {
    iomp_aio_t aio = job->aio;
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = read(aio->fildes, aio->buf + aio->offset, todo);
        if (len > 0) {
            aio->offset += len;
            if (len == todo) {
                free(job);
                aio->complete(aio, 0);
                break;
            }
        } else if (len == -1 && errno == EAGAIN) {
            free(job);
            if (iomp_queue_read(thread->queue, aio) == -1) {
                aio->complete(aio, errno);
            }
            break;
        } else {
            free(job);
            aio->complete(aio, (len == -1 ? errno : -1));
            break;
        }
    }
}

void do_write(iomp_aiojb_t job, iomp_thread_t thread) {
    iomp_aio_t aio = job->aio;
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = write(aio->fildes, aio->buf + aio->offset, todo);
        if (len > 0) {
            aio->offset += len;
            if (len == todo) {
                free(job);
                aio->complete(aio, 0);
                break;
            }
        } else if (len == -1 && errno == EAGAIN) {
            free(job);
            if (iomp_queue_write(thread->queue, aio) == -1) {
                aio->complete(aio, errno);
            }
            break;
        } else {
            free(job);
            aio->complete(aio, (len == -1 ? errno : -1));
            break;
        }
    }
}

int get_ncpu() {
    int ncpu = -1;
#if defined(__BSD__)
    int mib[2] = { CTL_HW, HW_NCPU };
    size_t len = sizeof(ncpu);
    int rv = sysctl(mib, 2, &ncpu, &len, NULL, 0);
    if (rv != 0) {
        IOMP_LOG(ERROR, "sysctlbyname fail: %s", strerror(rv));
        ncpu = -1;
    }
#elif defined(__linux__)
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu == -1) {
        IOMP_LOG(ERROR, "sysconf fail: %s", strerror(errno));
    }
#endif
    return ncpu;
}

