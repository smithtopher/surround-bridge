// Minimal OS layer for the portable engine: mutex / condition variable / thread,
// a monotonic nanosecond clock and sleep. POSIX (macOS, Linux) and Win32.
//
// pc_now_ns() must use the same timebase as the audio device back-end's
// presentation timestamps (macOS: mach host time; Windows: QueryPerformanceCounter,
// which is what IAudioClock::GetPosition's qpc position reports).
#ifndef PC_PLATFORM_H
#define PC_PLATFORM_H

#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
// ---------------------------------------------------------------- Win32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef SRWLOCK pc_mutex;
typedef CONDITION_VARIABLE pc_cond;
typedef HANDLE pc_thread;

static inline void pc_mutex_init(pc_mutex *m) { InitializeSRWLock(m); }
static inline void pc_mutex_destroy(pc_mutex *m) { (void)m; }
static inline void pc_mutex_lock(pc_mutex *m) { AcquireSRWLockExclusive(m); }
static inline void pc_mutex_unlock(pc_mutex *m) { ReleaseSRWLockExclusive(m); }

static inline void pc_cond_init(pc_cond *c) { InitializeConditionVariable(c); }
static inline void pc_cond_destroy(pc_cond *c) { (void)c; }
static inline void pc_cond_wait(pc_cond *c, pc_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static inline void pc_cond_signal(pc_cond *c) { WakeConditionVariable(c); }
static inline void pc_cond_broadcast(pc_cond *c) { WakeAllConditionVariable(c); }

typedef struct { void *(*fn)(void *); void *arg; } pc_thread_start_;
static DWORD WINAPI pc_thread_trampoline_(LPVOID p) {
    pc_thread_start_ s = *(pc_thread_start_ *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static inline int pc_thread_create(pc_thread *t, void *(*fn)(void *), void *arg) {
    pc_thread_start_ *s = (pc_thread_start_ *)malloc(sizeof(*s));
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, pc_thread_trampoline_, s, 0, NULL);
    if (!*t) { free(s); return -1; }
    return 0;
}
static inline void pc_thread_join(pc_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline int64_t pc_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    // split to avoid overflow and keep integer precision
    return (int64_t)(c.QuadPart / freq.QuadPart) * 1000000000LL +
           (int64_t)(c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart;
}

// Sleep() has 1 ms granularity at best; the app should call timeBeginPeriod(1).
static inline void pc_sleep_us(int64_t us) { Sleep((DWORD)((us + 999) / 1000)); }

#else
// ---------------------------------------------------------------- POSIX
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

typedef pthread_mutex_t pc_mutex;
typedef pthread_cond_t pc_cond;
typedef pthread_t pc_thread;

static inline void pc_mutex_init(pc_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void pc_mutex_destroy(pc_mutex *m) { pthread_mutex_destroy(m); }
static inline void pc_mutex_lock(pc_mutex *m) { pthread_mutex_lock(m); }
static inline void pc_mutex_unlock(pc_mutex *m) { pthread_mutex_unlock(m); }

static inline void pc_cond_init(pc_cond *c) { pthread_cond_init(c, NULL); }
static inline void pc_cond_destroy(pc_cond *c) { pthread_cond_destroy(c); }
static inline void pc_cond_wait(pc_cond *c, pc_mutex *m) { pthread_cond_wait(c, m); }
static inline void pc_cond_signal(pc_cond *c) { pthread_cond_signal(c); }
static inline void pc_cond_broadcast(pc_cond *c) { pthread_cond_broadcast(c); }

static inline int pc_thread_create(pc_thread *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1;
}
static inline void pc_thread_join(pc_thread t) { pthread_join(t, NULL); }

static inline int64_t pc_now_ns(void) {
#ifdef __APPLE__
    // mach host time: the timebase of CoreAudio's AudioTimeStamp.mHostTime
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return (int64_t)((t / tb.denom) * tb.numer + (t % tb.denom) * tb.numer / tb.denom);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

static inline void pc_sleep_us(int64_t us) { usleep((useconds_t)us); }

#endif
#endif
