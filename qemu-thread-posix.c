/*
 * Wrappers around mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Author:
 *  Marcelo Tosatti <mtosatti@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "qemu-thread.h"

static pthread_t pthread_null;

static void error_exit(int err, const char *msg)
{
    fprintf(stderr, "qemu: %s: %s\n", msg, strerror(err));
    abort();
}

void qemu_mutex_init(QemuMutex *mutex)
{
    int err;
    pthread_mutexattr_t mutexattr;

    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_ERRORCHECK);
    err = pthread_mutex_init(&mutex->lock, &mutexattr);
    pthread_mutexattr_destroy(&mutexattr);
    mutex->owner = pthread_null;
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_destroy(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_lock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_lock(&mutex->lock);
    mutex->owner = pthread_self();
    if (err)
        error_exit(err, __func__);
}

int qemu_mutex_trylock(QemuMutex *mutex)
{
    int err;
    err = pthread_mutex_trylock(&mutex->lock);
    if (err == 0) {
        mutex->owner = pthread_self();
    }

    return !!err;
}

void qemu_mutex_unlock(QemuMutex *mutex)
{
    int err;

    mutex->owner = pthread_null;
    err = pthread_mutex_unlock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_init(QemuCond *cond)
{
    int err;

    err = pthread_cond_init(&cond->cond, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_destroy(QemuCond *cond)
{
    int err;

    err = pthread_cond_destroy(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_signal(QemuCond *cond)
{
    int err;

    err = pthread_cond_signal(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    int err;

    err = pthread_cond_broadcast(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
{
    int err;

    assert(pthread_equal(mutex->owner, pthread_self()));
    mutex->owner = pthread_null;
    err = pthread_cond_wait(&cond->cond, &mutex->lock);
    mutex->owner = pthread_self();
    if (err)
        error_exit(err, __func__);
}

void qemu_thread_create(QemuThread *thread,
                       void *(*start_routine)(void*),
                       void *arg)
{
    int err;

    /* Leave signal handling to the iothread.  */
    sigset_t set, oldset;

    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &oldset);
    err = pthread_create(&thread->thread, NULL, start_routine, arg);
    if (err)
        error_exit(err, __func__);

    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
}

void qemu_thread_get_self(QemuThread *thread)
{
    thread->thread = pthread_self();
}

int qemu_thread_is_self(QemuThread *thread)
{
   return pthread_equal(pthread_self(), thread->thread);
}

void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}
