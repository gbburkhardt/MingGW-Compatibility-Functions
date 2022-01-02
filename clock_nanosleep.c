/*
  Released under MIT License

  Copyright (c) 2021 Glenn Burkhardt.

  Permission is hereby granted, free of charge, to any person obtaining a copy of
  this software and associated documentation files (the "Software"), to deal in
  the Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is furnished to do
  so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>
#include <stdbool.h>
#include "pthread.h"
#include "pthread_time.h"

// https://randomascii.wordpress.com/2020/10/04/windows-timer-resolution-the-great-rule-change/
// Feature available in Windows 2004 and later
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002

// Windows 2004, https://docs.microsoft.com/en-us/windows/release-health/release-information
#define Win2004_BUILD_NUMBER 19041  

/* Number of 100ns-seconds between the beginning of the Windows epoch
 * (Jan. 1, 1601) and the Unix epoch (Jan. 1, 1970)
 */
#define DELTA_EPOCH_IN_100NS    116444736000000000LL

#define POW10_9                 1000000000

static inline int lc_set_errno(int result)
{
    if (result != 0) {
        errno = result;
        return -1;
    }
    return 0;
}

static inline void timersub(struct timespec* c, 
                            const struct timespec* a,
                            const struct timespec* b)
{
    c->tv_sec = a->tv_sec - b->tv_sec;
    c->tv_nsec = a->tv_nsec - b->tv_nsec;
    if (c->tv_nsec < 0) {
        c->tv_sec--;
        c->tv_nsec += POW10_9;
    }
}

static inline void timeradd(struct timespec* c, 
                            const struct timespec* a,
                            const struct timespec* b)
{
    c->tv_sec = a->tv_sec + b->tv_sec;
    c->tv_nsec = a->tv_nsec + b->tv_nsec;
    if (c->tv_nsec >= POW10_9) {
        c->tv_sec++;
        c->tv_nsec -= POW10_9;
    }
}

static void printLastError(const char* func, int lineno)
{
    DWORD err = GetLastError();
    LPVOID lpMsgBuf=0;
    DWORD s = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                            FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                            0, err, 
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                            (LPTSTR)&lpMsgBuf, 0, 0);
    if (s) {
        fprintf(stderr, "%s:%d err=%ld: %s\n", func, lineno, err, (char*)lpMsgBuf);
        LocalFree(lpMsgBuf);
    } else {
        fprintf(stderr, "%s:%d err=%ld: unknown error\n", func, lineno, err);
    }
}

static inline
bool haveHighResTimer()
{
    char bn[32];
    DWORD sz = sizeof(bn);

    // Determine whether Windows version is at least Win 2004
    int s = RegGetValueA(HKEY_LOCAL_MACHINE, 
                         "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 
                         "CurrentBuildNumber", RRF_RT_REG_SZ, 0, bn, &sz);
    if (!s) {
        int curVer = atoi(bn);
        return (curVer >= Win2004_BUILD_NUMBER);
    }
    return false;
}    

/**
 * Sleep for the specified time.
 * @param  clock_id: CLOCK_REALTIME or CLOCK_MONOTONIC
 * @param  flags 0 for relative sleep interval, others for absolute waking up.
 * @param  request The desired sleep interval or absolute waking up time.
 * @param  remain The remain amount of time to sleep.
 *         The current implemention just ignore it.
 * @return If the function succeeds, the return value is 0.
 *         If the function fails, the return value is -1,
 *         with errno set to indicate the error.
 */
int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *request,
                    struct timespec *remain)
{
    struct timespec tp;

    switch (clock_id) {
      case CLOCK_REALTIME:

        /* TIMER_ABSTIME = 1 */
        if (flags == 0)
            return nanosleep(request, remain);

        // sleep until abs time
        clock_gettime(CLOCK_REALTIME, &tp);

        tp.tv_sec = request->tv_sec - tp.tv_sec;
        tp.tv_nsec = request->tv_nsec - tp.tv_nsec;
        if (tp.tv_nsec < 0) {
            tp.tv_nsec += POW10_9;
            tp.tv_sec --;
        }

        return nanosleep(&tp, remain);
    
      case CLOCK_MONOTONIC:
        if (request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= POW10_9) {
            return lc_set_errno(EINVAL);
        }
        // https://docs.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects

        // 100ns intervals, Positive values indicate absolute time.
        LARGE_INTEGER liDueTime;
        liDueTime.QuadPart = ((long long)request->tv_sec*POW10_9 + request->tv_nsec)/100;
        if (flags == 0) {
            liDueTime.QuadPart = -liDueTime.QuadPart;
        } else {
            liDueTime.QuadPart += DELTA_EPOCH_IN_100NS; // TIMER_ABSTIME
        }

        // Use high resolution if Windows version is at least Win 2004
        int timerCreateFlags;
        if (haveHighResTimer())
            timerCreateFlags = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
        else
            timerCreateFlags = CREATE_WAITABLE_TIMER_MANUAL_RESET;
            
        // Create an unnamed waitable timer.
        HANDLE hTimer = CreateWaitableTimerEx(0, 0, timerCreateFlags, TIMER_ALL_ACCESS);
        if (!hTimer) {
            printLastError(__func__, __LINE__);
            return lc_set_errno(ENOTSUP);
        }
    
        struct timespec then;
        if (remain) clock_gettime(CLOCK_MONOTONIC, &then);

        // Set a timer
        int retval = 0;
        if (!SetWaitableTimer(hTimer, &liDueTime, 0, 0, 0, 0)) {
            printLastError(__func__, __LINE__);
            retval = lc_set_errno(ENOTSUP);
        } else {
            // Wait for the timer.
            DWORD st = WaitForSingleObject(hTimer, INFINITE);
            if (st != WAIT_OBJECT_0) {
                printLastError(__func__, __LINE__);
                retval = lc_set_errno(ENOTSUP);
            }
        }
        
        CloseHandle(hTimer);

        struct timespec now, elapsed;
        if (remain) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            timersub(&elapsed, &now, &then);
            timersub(remain, request, &elapsed);
            if (remain->tv_sec < 0) {
                remain->tv_sec = 0;
                remain->tv_nsec = 0;
            }
        }

        return retval;

      default:
        return lc_set_errno(EINVAL);
    }
}

#ifdef UNIT_TEST
// gcc -DUNIT_TEST -g -O2 clock_nanosleep.c -o n -Wall -lwinmm

typedef NTSTATUS NTAPI (*NtQueryTimerResolution)(PULONG MinimumResolution,
                                                 PULONG MaximumResolution,
                                                 PULONG CurrentResolution);

// Returns the current timer resolution in 100 ns units (10,000 implies
// a one ms timer interval).
ULONG GetTimerResolution() {
  HMODULE ntdll = LoadLibraryA("ntdll.dll");
  NtQueryTimerResolution QueryTimerResolution
     = (NtQueryTimerResolution)GetProcAddress(ntdll, "NtQueryTimerResolution");

  ULONG minimum, maximum, current;
  QueryTimerResolution(&minimum, &maximum, &current);
  return current;
}

int main(int ac, char**av)
{
    int iterations = 10;
    struct timespec then, now, elapsed, remain;
    struct timespec delayTime = { .tv_sec=0, .tv_nsec=1000000 };

    if (ac > 1) delayTime.tv_nsec = atoi(av[1]);
    if (ac > 2) iterations = atoi(av[2]);
    
    if (haveHighResTimer())
        printf("High Res timer\n");
    else
        printf("Old windows version: no high res timer\n");

    ULONG ns100 = GetTimerResolution();
    double timerRes = ns100/10000.;
    printf("Timer resolution: %.6f ms\n", timerRes);

    TIMECAPS caps;    // use -lwinmm
    int s = timeGetDevCaps(&caps, sizeof(caps));
    if (s != TIMERR_NOERROR) perror("timeGetDevCaps");

    printf("caps: min %d ms, max %d ms\n", caps.wPeriodMin, caps.wPeriodMax);

    if (caps.wPeriodMin > timerRes) {
        printf("No reason to set min timer resolution\n");
    } else {
        timeBeginPeriod(caps.wPeriodMin);
        printf("Timer res set to minimum\n");
    }
    
    clock_gettime(CLOCK_MONOTONIC, &then);

    for (int i=0; i < iterations; i++) {
        int s = clock_nanosleep(CLOCK_MONOTONIC, 0, &delayTime, &remain);
        if (s) { perror("clock_nanosleep"); break; }
    }

    clock_gettime(CLOCK_MONOTONIC, &now);

    timersub(&elapsed, &now, &then);

    printf("remain %jd sec, %ld ns\n", remain.tv_sec, remain.tv_nsec);
    printf("elapsed  %.7f sec\n", elapsed.tv_sec + elapsed.tv_nsec*1./POW10_9);

    double dt = delayTime.tv_nsec*1./POW10_9;
    printf("expected %.7f sec, %d iterations, %.7f sec delay\n", 
           iterations*dt, iterations, dt);

    // wait until absolute time
    long long waitTime = delayTime.tv_sec * POW10_9 + delayTime.tv_nsec;
    waitTime *= iterations;
    delayTime.tv_sec = waitTime/(double)POW10_9;
    delayTime.tv_nsec = waitTime - delayTime.tv_sec*(long long)POW10_9;
    
    clock_gettime(CLOCK_MONOTONIC, &then);

    struct timespec until;
    timeradd(&until, &then, &delayTime);

    s = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &delayTime, &remain);
    if (s) perror("clock_nanosleep");
    
    clock_gettime(CLOCK_MONOTONIC, &now);

    printf("\n wait until abstime\n");
    printf("now:      %jd sec %ld nsec\n", now.tv_sec, now.tv_nsec);
    printf("expected: %jd sec %ld nsec\n", until.tv_sec, until.tv_nsec);
    printf("remain    %jd sec, %ld ns\n", remain.tv_sec, remain.tv_nsec);
    
    return 0;
}
#endif
