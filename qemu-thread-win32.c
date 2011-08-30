/*
 * Win32 implementation for mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2010
 *
 * Author:
 *  Paolo Bonzini <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "qemu-common.h"
#include "qemu-thread.h"
#include <process.h>
#include <assert.h>
#include <limits.h>

static void error_exit(int err, const char *msg)
{
    char *pstr;

    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                  NULL, err, 0, (LPTSTR)&pstr, 2, NULL);
    fprintf(stderr, "qemu: %s: %s\n", msg, pstr);
    LocalFree(pstr);
    abort();
}

void qemu_mutex_init(QemuMutex *mutex)
{
    mutex->count = 0;
    mutex->sema = NULL;
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    assert(mutex->count == 0);
    assert(mutex->owner == 0);
    if (mutex->sema) {
        CloseHandle(mutex->sema);
    }
}

HANDLE qemu_mutex_semaphore(QemuMutex *mutex)
{
    if (mutex->sema == NULL) {
        HANDLE sema = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
        if (!InterlockedCompareExchangePointer(&mutex->sema, sema, NULL)) {
            return sema;
        }

        /* Somebody beat us to it.  */
        CloseHandle(sema);
    }
    return mutex->sema;
}

void qemu_mutex_lock(QemuMutex *mutex)
{
    assert(mutex->owner != GetCurrentThreadId());
    if (InterlockedIncrement(&mutex->count) != 1) {
        WaitForSingleObject(qemu_mutex_semaphore(mutex), INFINITE);
    }
    mutex->owner = GetCurrentThreadId();
}

int qemu_mutex_lock_recursive(QemuMutex *mutex)
{
    int err;

    EnterCriticalSection(&mutex->lock);
    if (mutex->owner != 0) {
        /* decrement the recursion count */
        LeaveCriticalSection(&mutex->lock);
        return 1;
    }
    mutex->owner = GetCurrentThreadId();
    return 0;
}

int qemu_mutex_trylock(QemuMutex *mutex)
{
    assert(mutex->owner != GetCurrentThreadId());
    if (InterlockedCompareExchange(&mutex->count, 1, 0) == 0) {
        mutex->owner = GetCurrentThreadId();
	return false;
    } else {
	return true;
    }
}

void qemu_mutex_unlock(QemuMutex *mutex)
{
    assert(mutex->owner == GetCurrentThreadId());
    mutex->owner = 0;
    if (InterlockedDecrement(&mutex->count) != 0) {
        ReleaseSemaphore(qemu_mutex_semaphore(mutex), 1, NULL);
    }
}

void qemu_rwmutex_init(QemuRWMutex *mutex)
{
    InitializeCriticalSection(&mutex->readerCountLock);
    InitializeCriticalSection(&mutex->writerLock);

    /* A manual-reset event that we use as a poor man condition variable.  */
    mutex->noReaders = CreateEvent (NULL, TRUE, TRUE, NULL);
}

void qemu_rwmutex_rdlock(QemuRWMutex *mutex)
{
    assert(mutex->writer != GetCurrentThreadId());

    /*
     * We need to lock the writerLock too, otherwise a writer could
     * do the whole of qemu_rwmutex_wrlock after the readerCount changed
     * from 0 to 1, but before the event was reset.
     */
    EnterCriticalSection(&mutex->writerLock);
    EnterCriticalSection(&mutex->readerCountLock);
    if (++mutex->readerCount == 1) {
	/*
         * This makes the rwlock pretty expensive if there's little
	 * or no contention between readers.  But if this is the
	 * case, you might as well use a normal mutex.
	 */
        ResetEvent(mutex->noReaders);
    }
    LeaveCriticalSection(&mutex->readerCountLock);
    LeaveCriticalSection(&mutex->writerLock);
}

int qemu_rwmutex_wrlock(QemuRWMutex *mutex)
{
    int owned;

    EnterCriticalSection(&mutex->writerLock);
    /*
     * readerCount cannot change from 0 to 1 here.  If it changes from
     * 1 to 0, we'll just do a useless syscall.
     */
    if (mutex->readerCount > 0) {
	WaitForSingleObject(mutex->noReaders, INFINITE);
    }

    assert(mutex->writer == NULL);
    mutex->writer = GetCurrentThreadId();

    /* writerLock remains locked.  */
}

void qemu_rwmutex_unlock(QemuRWMutex *mutex)
{
    /*
     * For the writer, mutex->writer is read and written under the
     * writerLock, so it's safe.
     *
     * But mutex->writer is also protected for the reader, because while
     * there are readers (and we are one) mutex->noReaders is reset and
     * no thread can proceed to write mutex->writer.
     */
    if (mutex->writer != GetCurrentThreadId()) {
        /* We are a reader.  */
        assert (mutex->readerCount > 0);
        EnterCriticalSection(&mutex->readerCountLock);
        if (--mutex->readerCount == 0) {
            SetEvent(mutex->noReaders);
        }
        LeaveCriticalSection(&mutex->readerCountLock);

    } else {
        /* We are the writer.  */
        mutex->writer = NULL;
        LeaveCriticalSection(&mutex->writerLock);
    }
}

void qemu_cond_init(QemuCond *cond, QemuMutex *mutex)
{
    cond->mutex = mutex;
    cond->waiters = 0;
    cond->target = 0;

    cond->sema = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!cond->sema) {
        error_exit(GetLastError(), __func__);
    }
    cond->continue_event = CreateEvent(NULL,    /* security */
                                       FALSE,   /* auto-reset */
                                       FALSE,   /* not signaled */
                                       NULL);   /* name */
    if (!cond->continue_event) {
        error_exit(GetLastError(), __func__);
    }
}

void qemu_cond_destroy(QemuCond *cond)
{
    BOOL result;
    result = CloseHandle(cond->continue_event);
    if (!result) {
        error_exit(GetLastError(), __func__);
    }
    cond->continue_event = 0;
    result = CloseHandle(cond->sema);
    if (!result) {
        error_exit(GetLastError(), __func__);
    }
    cond->sema = 0;
}

void qemu_cond_signal(QemuCond *cond)
{
    DWORD result;
    assert(cond->mutex->owner == GetCurrentThreadId());

    /*
     * Signal only when there are waiters.  cond->waiters is
     * incremented by pthread_cond_wait under the external lock,
     * so we are safe about that.
     */
    if (cond->waiters == 0) {
        return;
    }

    /*
     * Waiting threads decrement it outside the external lock, but
     * only if another thread is executing pthread_cond_broadcast and
     * has the mutex.  So, it also cannot be decremented concurrently
     * with this particular access.
     */
    cond->target = cond->waiters - 1;
    result = SignalObjectAndWait(cond->sema, cond->continue_event,
                                 INFINITE, FALSE);
    if (result == WAIT_ABANDONED || result == WAIT_FAILED) {
        error_exit(GetLastError(), __func__);
    }
}

void qemu_cond_broadcast(QemuCond *cond)
{
    BOOLEAN result;
    /*
     * As in pthread_cond_signal, access to cond->waiters and
     * cond->target is locked via the external mutex.
     */
    assert(cond->mutex->owner == GetCurrentThreadId());
    if (cond->waiters == 0) {
        return;
    }

    cond->target = 0;
    result = ReleaseSemaphore(cond->sema, cond->waiters, NULL);
    if (!result) {
        error_exit(GetLastError(), __func__);
    }

    /*
     * At this point all waiters continue. Each one takes its
     * slice of the semaphore. Now it's our turn to wait: Since
     * the external mutex is held, no thread can leave cond_wait,
     * yet. For this reason, we can be sure that no thread gets
     * a chance to eat *more* than one slice. OTOH, it means
     * that the last waiter must send us a wake-up.
     */
    WaitForSingleObject(cond->continue_event, INFINITE);
}

void qemu_cond_wait(QemuCond *cond)
{
    /*
     * This access is protected under the mutex.
     */
    cond->waiters++;

    /*
     * Unlock external mutex and wait for signal.
     * NOTE: we've held mutex locked long enough to increment
     * waiters count above, so there's no problem with
     * leaving mutex unlocked before we wait on semaphore.
     */
    qemu_mutex_unlock(cond->mutex);

    WaitForSingleObject(cond->sema, INFINITE);

    /* Now waiters must rendez-vous with the signaling thread and
     * let it continue.  For cond_broadcast this has heavy contention
     * and triggers thundering herd.  So goes life.
     *
     * Decrease waiters count.  The mutex is not taken, so we have
     * to do this atomically.
     *
     * All waiters contend for the mutex at the end of this function
     * until the signaling thread relinquishes it.  To ensure
     * each waiter consumes exactly one slice of the semaphore,
     * the signaling thread stops until it is told by the last
     * waiter that it can go on.
     */
    if (InterlockedDecrement(&cond->waiters) == cond->target) {
        SetEvent(cond->continue_event);
    }

    /* lock external mutex again */
    qemu_mutex_lock(cond->mutex);
}

void qemu_sem_init(QemuSemaphore *sem, int init)
{
    /* Manual reset.  */
    sem->sema = CreateSemaphore(NULL, init, LONG_MAX, NULL);
}

void qemu_sem_destroy(QemuSemaphore *sem)
{
    CloseHandle(sem->sema);
}

void qemu_sem_post(QemuSemaphore *sem)
{
    ReleaseSemaphore(sem->sema, 1, NULL);
}

int qemu_sem_timedwait(QemuSemaphore *sem, int ms)
{
    int rc = WaitForSingleObject(sem->sema, ms);
    if (rc == WAIT_OBJECT_0) {
        return 0;
    }
    if (rc != WAIT_TIMEOUT) {
        error_exit(GetLastError(), __func__);
    }
    return -1;
}

void qemu_sem_wait(QemuSemaphore *sem)
{
    if (WaitForSingleObject(sem->sema, INFINITE) != WAIT_OBJECT_0) {
        error_exit(GetLastError(), __func__);
    }
}

struct QemuThreadData {
    /* Passed to win32_start_routine.  */
    void             *(*start_routine)(void *);
    void             *arg;
    short             mode;

    /* Only used for joinable threads. */
    bool              exited;
    void             *ret;
    CRITICAL_SECTION  cs;
};

static int qemu_thread_tls_index = TLS_OUT_OF_INDEXES;

static unsigned __stdcall win32_start_routine(void *arg)
{
    QemuThreadData *data = (QemuThreadData *) arg;
    void *(*start_routine)(void *) = data->start_routine;
    void *thread_arg = data->arg;

    if (data->mode == QEMU_THREAD_DETACHED) {
        g_free(data);
        data = NULL;
    }
    TlsSetValue(qemu_thread_tls_index, data);
    qemu_thread_exit(start_routine(thread_arg));
    abort();
}

void qemu_thread_exit(void *arg)
{
    QemuThreadData *data = TlsGetValue(qemu_thread_tls_index);
    if (data) {
        assert(data->mode != QEMU_THREAD_DETACHED);
        data->ret = arg;
        EnterCriticalSection(&data->cs);
        data->exited = true;
        LeaveCriticalSection(&data->cs);
    }
    _endthreadex(0);
}

void *qemu_thread_join(QemuThread *thread)
{
    QemuThreadData *data;
    void *ret;
    HANDLE handle;

    data = thread->data;
    if (!data) {
        return NULL;
    }
    /*
     * Because multiple copies of the QemuThread can exist via
     * qemu_thread_get_self, we need to store a value that cannot
     * leak there.  The simplest, non racy way is to store the TID,
     * discard the handle that _beginthreadex gives back, and
     * get another copy of the handle here.
     */
    handle = qemu_thread_get_handle(thread);
    if (handle) {
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }
    ret = data->ret;
    assert(data->mode != QEMU_THREAD_DETACHED);
    DeleteCriticalSection(&data->cs);
    g_free(data);
    return ret;
}

static inline void qemu_thread_init(void)
{
    if (qemu_thread_tls_index == TLS_OUT_OF_INDEXES) {
        qemu_thread_tls_index = TlsAlloc();
        if (qemu_thread_tls_index == TLS_OUT_OF_INDEXES) {
            error_exit(ERROR_NO_SYSTEM_RESOURCES, __func__);
        }
    }
}


void qemu_thread_create(QemuThread *thread,
                       void *(*start_routine)(void *),
                       void *arg, int mode)
{
    HANDLE hThread;

    struct QemuThreadData *data;
    qemu_thread_init();
    data = g_malloc(sizeof *data);
    data->start_routine = start_routine;
    data->arg = arg;
    data->mode = mode;
    data->exited = false;

    if (data->mode != QEMU_THREAD_DETACHED) {
        InitializeCriticalSection(&data->cs);
    }

    hThread = (HANDLE) _beginthreadex(NULL, 0, win32_start_routine,
                                      data, 0, &thread->tid);
    if (!hThread) {
        error_exit(GetLastError(), __func__);
    }
    CloseHandle(hThread);
    thread->data = (mode == QEMU_THREAD_DETACHED) ? NULL : data;
}

void qemu_thread_get_self(QemuThread *thread)
{
    qemu_thread_init();
    thread->data = TlsGetValue(qemu_thread_tls_index);
    thread->tid = GetCurrentThreadId();
}

HANDLE qemu_thread_get_handle(QemuThread *thread)
{
    QemuThreadData *data;
    HANDLE handle;

    data = thread->data;
    if (!data) {
        return NULL;
    }

    assert(data->mode != QEMU_THREAD_DETACHED);
    EnterCriticalSection(&data->cs);
    if (!data->exited) {
        handle = OpenThread(SYNCHRONIZE | THREAD_SUSPEND_RESUME, FALSE,
                            thread->tid);
    } else {
        handle = NULL;
    }
    LeaveCriticalSection(&data->cs);
    return handle;
}

bool qemu_thread_is_self(QemuThread *thread)
{
    return GetCurrentThreadId() == thread->tid;
}
