#pragma once

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <cstdint>

inline void pinThisThread(int cpu) {
#ifdef __linux__
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<unsigned>(cpu), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)cpu;
#endif
}

inline bool setRealtimeFifo(int prio) {
#ifdef __linux__
    struct sched_param sp{};
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
#else
    (void)prio;
    return false;
#endif
}
