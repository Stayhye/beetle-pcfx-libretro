/* Copyright  (C) 2010-2020 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (rthreads.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef __unix__
#ifndef __sun__
#define _POSIX_C_SOURCE 199309
#endif
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <boolean.h>
#include <rthreads/rthreads.h>

/* with RETRO_WIN32_USE_PTHREADS, pthreads can be used even on win32. Maybe only supported in MSVC>=2005  */

#if defined(_WIN32) && !defined(RETRO_WIN32_USE_PTHREADS)
#define USE_WIN32_THREADS
#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500 /*_WIN32_WINNT_WIN2K */
#endif
#include <windows.h>
#include <mmsystem.h>
#endif
#elif defined(GEKKO)
#include <ogc/lwp_watchdog.h>
#include "gx_pthread.h"
#elif defined(_3DS)
#include "ctr_pthread.h"
#else
#include <pthread.h>
#include <time.h>
#endif

#if defined(VITA) || defined(BSD) || defined(ORBIS) || defined(__mips__) || defined(_3DS)
#include <sys/time.h>
#endif

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#ifdef PS2
static int ps2_clock(void)
{
   return (int)(clock() / (CLOCKS_PER_SEC / 1000));
}
#endif

struct thread_data
{
   void (*func)(void*);
   void *userdata;
};

struct sthread
{
#ifdef USE_WIN32_THREADS
   HANDLE thread;
   DWORD id;
#else
   pthread_t id;
#endif
};

struct slock
{
#ifdef USE_WIN32_THREADS
   CRITICAL_SECTION lock;
#else
   pthread_mutex_t lock;
#endif
};

#ifdef USE_WIN32_THREADS
/* The syntax we'll use is mind-bending unless we use a struct. Plus, we might want to store more info later */
/* This will be used as a linked list immplementing a queue of waiting threads */
struct queue_entry
{
   struct queue_entry *next;
};
#endif

struct scond
{
#ifdef USE_WIN32_THREADS
   /* With this implementation of scond, we don't have any way of waking
    * (or even identifying) specific threads
    * But we need to wake them in the order indicated by the queue.
    * This potato token will get get passed around every waiter.
    * The bearer can test whether he's next, and hold onto the potato if he is.
    * When he's done he can then put it back into play to progress
    * the queue further */
   HANDLE hot_potato;

   /* The primary signalled event. Hot potatoes are passed until this is set. */
   HANDLE event;

   /* the head of the queue; NULL if queue is empty */
   struct queue_entry *head;

   /* equivalent to the queue length */
   int waiters;

   /* how many waiters in the queue have been conceptually wakened by signals
    * (even if we haven't managed to actually wake them yet) */
   int wakens;

   /* used to control access to this scond, in case the user fails */
   CRITICAL_SECTION cs;

#else
   pthread_cond_t cond;
#endif
};

#ifdef USE_WIN32_THREADS
static DWORD CALLBACK thread_wrap(void *data_)
#else
static void *thread_wrap(void *data_)
#endif
{
   struct thread_data *data = (struct thread_data*)data_;
   if (!data)
      return 0;
   data->func(data->userdata);
   free(data);
   return 0;
}

/**
 * sthread_create:
 * @start_routine         : thread entry callback function
 * @userdata              : pointer to userdata that will be made
 *                          available in thread entry callback function
 *
 * Create a new thread.
 *
 * Returns: pointer to new thread if successful, otherwise NULL.
 */
sthread_t *sthread_create(void (*thread_func)(void*), void *userdata)
{
    return sthread_create_with_priority(thread_func, userdata, 0);
}

/* TODO/FIXME - this needs to be implemented for Switch/3DS */
#if !defined(SWITCH) && !defined(USE_WIN32_THREADS) && !defined(_3DS) && !defined(GEKKO) && !defined(__HAIKU__) && !defined(EMSCRIPTEN)
#define HAVE_THREAD_ATTR
#endif

/**
 * sthread_create_with_priority:
 * @start_routine         : thread entry callback function
 * @userdata              : pointer to userdata that will be made
 *                          available in thread entry callback function
 * @thread_priority       : thread priority hint value from [1-100]
 *
 * Create a new thread. It is possible for the caller to give a hint
 * for the thread's priority from [1-100]. Any passed in @thread_priority
 * values that are outside of this range will cause sthread_create() to
 * create a new thread using the operating system's default thread
 * priority.
 *
 * Returns: pointer to new thread if successful, otherwise NULL.
 */
sthread_t *sthread_create_with_priority(void (*thread_func)(void*), void *userdata, int thread_priority)
{
#ifdef HAVE_THREAD_ATTR
   pthread_attr_t thread_attr;
   bool thread_attr_needed  = false;
#endif
   bool thread_created      = false;
   struct thread_data *data = NULL;
   sthread_t *thread        = (sthread_t*)malloc(sizeof(*thread));

   if (!thread)
      return NULL;

   if (!(data = (struct thread_data*)malloc(sizeof(*data))))
   {
      free(thread);
      return NULL;
   }

   data->func               = thread_func;
   data->userdata           = userdata;

   thread->id               = 0;
#ifdef USE_WIN32_THREADS
   thread->thread           = CreateThread(NULL, 0, thread_wrap,
         data, 0, &thread->id);
   thread_created           = !!thread->thread;
#else
#ifdef HAVE_THREAD_ATTR
   pthread_attr_init(&thread_attr);

   if ((thread_priority >= 1) && (thread_priority <= 100))
   {
      struct sched_param sp;
      memset(&sp, 0, sizeof(struct sched_param));
      sp.sched_priority = thread_priority;
      pthread_attr_setschedpolicy(&thread_attr, SCHED_RR);
      pthread_attr_setschedparam(&thread_attr, &sp);

      thread_attr_needed = true;
   }

#if defined(VITA)
   pthread_attr_setstacksize(&thread_attr , 0x10000 );
   thread_attr_needed = true;
#elif defined(__APPLE__)
   /* Default stack size on Apple is 512Kb; 
    * for PS2 disc scanning and other reasons, we'd like 2MB. */
   pthread_attr_setstacksize(&thread_attr , 0x200000 );
   thread_attr_needed = true;
#endif

   if (thread_attr_needed)
      thread_created = pthread_create(&thread->id, &thread_attr, thread_wrap, data) == 0;
   else
      thread_created = pthread_create(&thread->id, NULL, thread_wrap, data) == 0;

   pthread_attr_destroy(&thread_attr);
#else
   thread_created    = pthread_create(&thread->id, NULL, thread_wrap, data) == 0;
#endif

#endif

   if (thread_created)
      return thread;
   free(data);
   free(thread);
   return NULL;
}

/**
 * sthread_detach:
 * @thread                : pointer to thread object
 *
 * Detach a thread. When a detached thread terminates, its
 * resources are automatically released back to the system
 * without the need for another thread to join with the
 * terminated thread.
 *
 * Returns: 0 on success, otherwise it returns a non-zero error number.
 */
int sthread_detach(sthread_t *thread)
{
#ifdef USE_WIN32_THREADS
   CloseHandle(thread->thread);
   free(thread);
   return 0;
#else
   int ret = pthread_detach(thread->id);
   free(thread);
   return ret;
#endif
}

/**
 * sthread_join:
 * @thread                : pointer to thread object
 *
 * Join with a terminated thread. Waits for the thread specified by
 * @thread to terminate. If that thread has already terminated, then
 * it will return immediately. The thread specified by @thread must
 * be joinable.
 *
 * Returns: 0 on success, otherwise it returns a non-zero error number.
 */
void sthread_join(sthread_t *thread)
{
   if (!thread)
      return;
#ifdef USE_WIN32_THREADS
   WaitForSingleObject(thread->thread, INFINITE);
   CloseHandle(thread->thread);
#else
   pthread_join(thread->id, NULL);
#endif
   free(thread);
}

#if !defined(GEKKO)
/**
 * sthread_isself:
 * @thread                : pointer to thread object
 *
 * Returns: true (1) if calling thread is the specified thread
 */
bool sthread_isself(sthread_t *thread)
{
#ifdef USE_WIN32_THREADS
   return thread ? GetCurrentThreadId() == thread->id         : false;
#else
   return thread ? pthread_equal(pthread_self(), thread->id) : false;
#endif
}
#endif

/**
 * slock_new:
 *
 * Create and initialize a new mutex. Must be manually
 * freed.
 *
 * Returns: pointer to a new mutex if successful, otherwise NULL.
 **/
slock_t *slock_new(void)
{
   slock_t       *lock = (slock_t*)calloc(1, sizeof(*lock));
   if (!lock)
      return NULL;
#ifdef USE_WIN32_THREADS
   InitializeCriticalSection(&lock->lock);
#else
   if (pthread_mutex_init(&lock->lock, NULL) != 0)
   {
      free(lock);
      return NULL;
   }
#endif
   return lock;
}

/**
 * slock_free:
 * @lock                  : pointer to mutex object
 *
 * Frees a mutex.
 **/
void slock_free(slock_t *lock)
{
   if (!lock)
      return;

#ifdef USE_WIN32_THREADS
   DeleteCriticalSection(&lock->lock);
#else
   pthread_mutex_destroy(&lock->lock);
#endif
   free(lock);
}

/**
 * slock_lock:
 * @lock                  : pointer to mutex object
 *
 * Locks a mutex. If a mutex is already locked by
 * another thread, the calling thread shall block until
 * the mutex becomes available.
**/
void slock_lock(slock_t *lock)
{
   if (!lock)
      return;
#ifdef USE_WIN32_THREADS
   EnterCriticalSection(&lock->lock);
#else
   pthread_mutex_lock(&lock->lock);
#endif
}

/**
 * slock_try_lock:
 * @lock                  : pointer to mutex object
 *
 * Attempts to lock a mutex. If a mutex is already locked by
 * another thread, return false.  If the lock is acquired, return true.
**/
bool slock_try_lock(slock_t *lock)
{
#ifdef USE_WIN32_THREADS
   return lock && TryEnterCriticalSection(&lock->lock);
#else
   return lock && (pthread_mutex_trylock(&lock->lock) == 0);
#endif
}

/**
 * slock_unlock:
 * @lock                  : pointer to mutex object
 *
 * Unlocks a mutex.
 **/
void slock_unlock(slock_t *lock)
{
   if (!lock)
      return;
#ifdef USE_WIN32_THREADS
   LeaveCriticalSection(&lock->lock);
#else
   pthread_mutex_unlock(&lock->lock);
#endif
}

/**
 * scond_new:
 *
 * Creates and initializes a condition variable. Must
 * be manually freed.
 *
 * Returns: pointer to new condition variable on success,
 * otherwise NULL.
 **/
scond_t *scond_new(void)
{
   scond_t       *cond = (scond_t*)calloc(1, sizeof(*cond));

   if (!cond)
      return NULL;

#ifdef USE_WIN32_THREADS
   if (!(cond->event      = CreateEvent(NULL, FALSE, FALSE, NULL)))
      goto error;
   if (!(cond->hot_potato = CreateEvent(NULL, FALSE, FALSE, NULL)))
   {
      CloseHandle(cond->event);
      goto error;
   }

   InitializeCriticalSection(&cond->cs);
#else
   if (pthread_cond_init(&cond->cond, NULL) != 0)
      goto error;
#endif

   return cond;

error:
   free(cond);
   return NULL;
}

/**
 * scond_free:
 * @cond                  : pointer to condition variable object
 *
 * Frees a condition variable.
**/
void scond_free(scond_t *cond)
{
   if (!cond)
      return;

#ifdef USE_WIN32_THREADS
   CloseHandle(cond->event);
   CloseHandle(cond->hot_potato);
   DeleteCriticalSection(&cond->cs);
#else
   pthread_cond_destroy(&cond->cond);
#endif
   free(cond);
}

#ifdef USE_WIN32_THREADS
static bool _scond_wait_win32(scond_t *cond, slock_t *lock, DWORD dwMilliseconds)
{
   struct queue_entry myentry;
   struct queue_entry **ptr;

#if _WIN32_WINNT >= 0x0500 || defined(_XBOX)
   static LARGE_INTEGER performanceCounterFrequency;
   LARGE_INTEGER tsBegin;
   static bool first_init  = true;
#else
   static bool beginPeriod = false;
   DWORD tsBegin;
#endif
   DWORD waitResult;
   DWORD dwFinalTimeout = dwMilliseconds;

   EnterCriticalSection(&cond->cs);

#if _WIN32_WINNT >= 0x0500 || defined(_XBOX)
   if (first_init)
   {
      performanceCounterFrequency.QuadPart = 0;
      first_init = false;
   }

   if (performanceCounterFrequency.QuadPart == 0)
      QueryPerformanceFrequency(&performanceCounterFrequency);
#else
   if (!beginPeriod)
   {
      beginPeriod = true;
      timeBeginPeriod(1);
   }
#endif

   if (dwMilliseconds != INFINITE)
#if _WIN32_WINNT >= 0x0500 || defined(_XBOX)
      QueryPerformanceCounter(&tsBegin);
#else
      tsBegin = timeGetTime();
#endif

   ptr = &cond->head;

   while (*ptr)
      ptr         = &((*ptr)->next);

   *ptr         = &myentry;
   myentry.next = NULL;

   cond->waiters++;

   while (cond->head != &myentry)
   {
      DWORD timeout = INFINITE;

      if (cond->wakens > 0)
         SetEvent(cond->hot_potato);

      if (dwMilliseconds != INFINITE)
      {
#if _WIN32_WINNT >= 0x0500 || defined(_XBOX)
         LARGE_INTEGER now;
         LONGLONG elapsed;

         QueryPerformanceCounter(&now);
         elapsed  = now.QuadPart - tsBegin.QuadPart;
         elapsed *= 1000;
         elapsed /= performanceCounterFrequency.QuadPart;
#else
         DWORD now     = timeGetTime();
         DWORD elapsed = now - tsBegin;
#endif

         if (elapsed > dwMilliseconds)
            elapsed = dwMilliseconds;

         timeout = dwMilliseconds - elapsed;
      }

      LeaveCriticalSection(&lock->lock);
      LeaveCriticalSection(&cond->cs);

      Sleep(0);
      waitResult = WaitForSingleObject(cond->hot_potato, timeout);

      EnterCriticalSection(&lock->lock);
      EnterCriticalSection(&cond->cs);

      if (waitResult == WAIT_TIMEOUT)
      {
         if (cond->head == &myentry)
         {
            dwFinalTimeout = 0;
            break;
         }
         else
         {
            struct queue_entry *curr = cond->head;

            while (curr->next != &myentry)
               curr = curr->next;
            curr->next = myentry.next;
            cond->waiters--;
            LeaveCriticalSection(&cond->cs);
            return false;
         }
      }

   }

   LeaveCriticalSection(&lock->lock);
   LeaveCriticalSection(&cond->cs);

   waitResult = WaitForSingleObject(cond->event, dwFinalTimeout);

   EnterCriticalSection(&lock->lock);
   EnterCriticalSection(&cond->cs);

   cond->head = myentry.next;
   cond->waiters--;

   if (waitResult == WAIT_TIMEOUT)
   {
      LeaveCriticalSection(&cond->cs);
      return false;
   }

   cond->wakens--;
   if (cond->wakens > 0)
   {
      SetEvent(cond->event);
      SetEvent(cond->hot_potato);
   }

   LeaveCriticalSection(&cond->cs);
   return true;
}
#endif

/**
 * scond_wait:
 * @cond                  : pointer to condition variable object
 * @lock                  : pointer to mutex object
 *
 * Block on a condition variable (i.e. wait on a condition).
 **/
void scond_wait(scond_t *cond, slock_t *lock)
{
#ifdef USE_WIN32_THREADS
   _scond_wait_win32(cond, lock, INFINITE);
#else
   pthread_cond_wait(&cond->cond, &lock->lock);
#endif
}

/**
 * scond_broadcast:
 * @cond                  : pointer to condition variable object
 *
 * Broadcast a condition. Unblocks all threads currently blocked
 * on the specified condition variable @cond.
 **/
int scond_broadcast(scond_t *cond)
{
#ifdef USE_WIN32_THREADS
   if (cond->waiters != 0)
   {
      if (cond->wakens == 0)
         SetEvent(cond->event);
      cond->wakens = cond->waiters;

      SetEvent(cond->hot_potato);
   }
   return 0;
#else
   return pthread_cond_broadcast(&cond->cond);
#endif
}

/**
 * scond_signal:
 * @cond                  : pointer to condition variable object
 *
 * Signal a condition. Unblocks at least one of the threads currently blocked
 * on the specified condition variable @cond.
 **/
void scond_signal(scond_t *cond)
{
#ifdef USE_WIN32_THREADS
   EnterCriticalSection(&cond->cs);

   if (cond->waiters == 0)
   {
      LeaveCriticalSection(&cond->cs);
      return;
   }

   if (cond->wakens == 0)
      SetEvent(cond->event);

   cond->wakens++;

   LeaveCriticalSection(&cond->cs);

   SetEvent(cond->hot_potato);
#else
   pthread_cond_signal(&cond->cond);
#endif
}

/**
 * scond_wait_timeout:
 * @cond                  : pointer to condition variable object
 * @lock                  : pointer to mutex object
 * @timeout_us            : timeout (in microseconds)
 *
 * Try to block on a condition variable (i.e. wait on a condition) until
 * @timeout_us elapses.
 *
 * Returns: false (0) if timeout elapses before condition variable is
 * signaled or broadcast, otherwise true (1).
 **/
bool scond_wait_timeout(scond_t *cond, slock_t *lock, int64_t timeout_us)
{
#ifdef USE_WIN32_THREADS
   if (timeout_us == 0)
      return false;
   else if (timeout_us < 1000)
      return _scond_wait_win32(cond, lock, 1);
   return _scond_wait_win32(cond, lock, timeout_us / 1000);
#else
   int64_t seconds, remainder;
   struct timespec now;
#ifdef __MACH__
   clock_serv_t cclock;
   mach_timespec_t mts;
   host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
   clock_get_time(cclock, &mts);
   mach_port_deallocate(mach_task_self(), cclock);
   now.tv_sec = mts.tv_sec;
   now.tv_nsec = mts.tv_nsec;
#elif !defined(__PSL1GHT__) && defined(__PS3__)
   sys_time_sec_t s;
   sys_time_nsec_t n;
   sys_time_get_current_time(&s, &n);
   now.tv_sec            = s;
   now.tv_nsec           = n;
#elif defined(PS2)
   int tickms            = ps2_clock();
   now.tv_sec            = tickms / 1000;
   now.tv_nsec           = (tickms % 1000) * 1000000;
#elif !defined(DINGUX_BETA) && (defined(__mips__) || defined(VITA) || defined(_3DS))
   struct timeval tm;
   gettimeofday(&tm, NULL);
   now.tv_sec            = tm.tv_sec;
   now.tv_nsec           = tm.tv_usec * 1000;
#elif defined(RETRO_WIN32_USE_PTHREADS)
   _ftime64_s(&now);
#elif defined(GEKKO)
   const uint64_t tickms = gettime() / TB_TIMER_CLOCK;
   now.tv_sec            = tickms / 1000;
   now.tv_nsec           = tickms * 1000;
#else
   clock_gettime(CLOCK_REALTIME, &now);
#endif

   seconds              = timeout_us / INT64_C(1000000);
   remainder            = timeout_us % INT64_C(1000000);

   now.tv_sec          += seconds;
   now.tv_nsec         += remainder * INT64_C(1000);

   if (now.tv_nsec > 1000000000)
   {
      now.tv_nsec      -= 1000000000;
      now.tv_sec       += 1;
   }

   return (pthread_cond_timedwait(&cond->cond, &lock->lock, &now) == 0);
#endif
}

#ifdef HAVE_THREAD_STORAGE
bool sthread_tls_create(sthread_tls_t *tls)
{
#ifdef USE_WIN32_THREADS
   return (*tls = TlsAlloc()) != TLS_OUT_OF_INDEXES;
#else
   return pthread_key_create((pthread_key_t*)tls, NULL) == 0;
#endif
}

bool sthread_tls_delete(sthread_tls_t *tls)
{
#ifdef USE_WIN32_THREADS
   return TlsFree(*tls) != 0;
#else
   return pthread_key_delete(*tls) == 0;
#endif
}

void *sthread_tls_get(sthread_tls_t *tls)
{
#ifdef USE_WIN32_THREADS
   return TlsGetValue(*tls);
#else
   return pthread_getspecific(*tls);
#endif
}

bool sthread_tls_set(sthread_tls_t *tls, const void *data)
{
#ifdef USE_WIN32_THREADS
   return TlsSetValue(*tls, (void*)data) != 0;
#else
   return pthread_setspecific(*tls, data) == 0;
#endif
}
#endif

uintptr_t sthread_get_thread_id(sthread_t *thread)
{
   if (thread)
      return (uintptr_t)thread->id;
   return 0;
}

uintptr_t sthread_get_current_thread_id(void)
{
#ifdef USE_WIN32_THREADS
   return (uintptr_t)GetCurrentThreadId();
#else
   return (uintptr_t)pthread_self();
#endif
}