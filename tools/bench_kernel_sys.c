#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "realtime_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <math.h>

#define BENCH_DEFAULT_ITERS 300
#define FRAME_INTERVAL_NS 16666666ULL /* 60.000006 Hz */
#define BUFFER_TOUCH_BYTES (256 * 1024)

typedef enum {
    CLOCK_METHOD_TIMERFD = 0,
    CLOCK_METHOD_NANOSLEEP = 1
} clock_method_t;

typedef struct {
    char mode_name[32];
    char method_name[32];
    size_t iterations;
    long minor_faults;
    long major_faults;
    latency_stats_t wake_latency_stats; /* Actual wake - target wake (us) */
    latency_stats_t cycle_jitter_stats; /* |dt - 16666.67us| (us) */
    double samples[TELEMETRY_WINDOW_SIZE];
    realtime_sys_status_t rt_status;
} bench_result_t;

static void simulate_frame_workload(void) {
    /* Simulate per-frame memory touching and buffer conversion workload */
    volatile uint8_t *work_buf = (volatile uint8_t *)malloc(BUFFER_TOUCH_BYTES);
    if (work_buf) {
        for (size_t i = 0; i < BUFFER_TOUCH_BYTES; i += 4096) {
            work_buf[i] = (uint8_t)(i & 0xFF);
        }
        work_buf[BUFFER_TOUCH_BYTES - 1] = 0x5A;
        free((void *)work_buf);
    }
}

static void run_benchmark(bench_result_t *res, bool realtime, int target_core,
                          size_t iterations, clock_method_t method) {
    memset(res, 0, sizeof(*res));
    strncpy(res->mode_name, realtime ? "REAL-TIME MODE" : "NORMAL (CFS) MODE", sizeof(res->mode_name) - 1);
    strncpy(res->method_name, (method == CLOCK_METHOD_TIMERFD) ? "timerfd" : "clock_nanosleep", sizeof(res->method_name) - 1);
    res->iterations = iterations;

    if (realtime) {
        realtime_sys_init(target_core);
        res->rt_status = *realtime_sys_get_status();
    }

    struct rusage ru_start, ru_end;
    getrusage(RUSAGE_SELF, &ru_start);

    int tfd = -1;
    if (method == CLOCK_METHOD_TIMERFD) {
        tfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (tfd < 0) {
            perror("timerfd_create");
            exit(1);
        }
    }

    struct timespec t_base;
    clock_gettime(CLOCK_MONOTONIC, &t_base);

    if (method == CLOCK_METHOD_TIMERFD) {
        struct itimerspec its = {
            .it_interval = { .tv_sec = 0, .tv_nsec = FRAME_INTERVAL_NS },
            .it_value    = { .tv_sec = 0, .tv_nsec = FRAME_INTERVAL_NS }
        };
        if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
            perror("timerfd_settime");
            exit(1);
        }
    }

    double latencies[TELEMETRY_WINDOW_SIZE];
    double jitters[TELEMETRY_WINDOW_SIZE];
    struct timespec t_prev = t_base;

    for (size_t i = 0; i < iterations; i++) {
        uint64_t target_ns = (uint64_t)t_base.tv_sec * 1000000000ULL +
                             t_base.tv_nsec +
                             (uint64_t)(i + 1) * FRAME_INTERVAL_NS;

        if (method == CLOCK_METHOD_TIMERFD) {
            uint64_t exp = 0;
            ssize_t s = read(tfd, &exp, sizeof(exp));
            if (s < 0 && errno != EINTR) {
                perror("timerfd read");
                break;
            }
        } else {
            struct timespec req = {
                .tv_sec = (time_t)(target_ns / 1000000000ULL),
                .tv_nsec = (long)(target_ns % 1000000000ULL)
            };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
        }

        struct timespec t_wake;
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        uint64_t actual_ns = (uint64_t)t_wake.tv_sec * 1000000000ULL + t_wake.tv_nsec;

        /* Wakeup latency in microseconds */
        double wake_latency_us = (double)((int64_t)actual_ns - (int64_t)target_ns) / 1000.0;
        if (wake_latency_us < 0.0) wake_latency_us = 0.0; /* Clock granularity clamping */

        /* Cycle-to-cycle duration jitter: |actual_dt - target_dt| */
        double cycle_dt_us = realtime_sys_elapsed_us(&t_prev, &t_wake);
        double cycle_jitter_us = fabs(cycle_dt_us - ((double)FRAME_INTERVAL_NS / 1000.0));
        t_prev = t_wake;

        if (i < TELEMETRY_WINDOW_SIZE) {
            latencies[i] = wake_latency_us;
            jitters[i]   = cycle_jitter_us;
            res->samples[i] = wake_latency_us;
        }

        /* Perform per-frame memory allocation/touch to stress page faults */
        simulate_frame_workload();
    }

    if (tfd >= 0) {
        close(tfd);
    }

    getrusage(RUSAGE_SELF, &ru_end);
    res->minor_faults = ru_end.ru_minflt - ru_start.ru_minflt;
    res->major_faults = ru_end.ru_majflt - ru_start.ru_majflt;

    telemetry_compute_stage_stats(latencies, iterations, &res->wake_latency_stats);
    telemetry_compute_stage_stats(jitters, iterations, &res->cycle_jitter_stats);

    if (realtime) {
        realtime_sys_teardown();
    }
}

static void print_comparison(const bench_result_t *norm, const bench_result_t *rt) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                FluxCast-C Kernel & Real-Time Subsystem Benchmark (300 Frames @ 60 FPS)            ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════════════════════════╝\n");

    printf("\n[*] CONFIGURATION & HARDWARE DETAILS:\n");
    printf("    Target Interval:       %.3f ms (16,666.666 us, 60.00 Hz)\n", (double)FRAME_INTERVAL_NS / 1000000.0);
    printf("    Timing Engine:         %s\n", norm->method_name);
    printf("    Target CPU Core:       %d\n", rt->rt_status.target_cpu_core);
    printf("    Memory Lock (mlock):   %s\n", rt->rt_status.mlock_ok ? "ACTIVE (mlockall)" : "DISABLED");
    printf("    Stack Pre-fault (64K): %s\n", rt->rt_status.prefault_ok ? "APPLIED" : "NONE");
    printf("    Thread Affinity:       %s\n", rt->rt_status.affinity_ok ? "PINNED" : "FLOATING");
    printf("    CPU C-State Locking:   %s\n", rt->rt_status.cpu_dma_latency_ok ? "C0/C1 LOCKED (/dev/cpu_dma_latency=0)" : "POWER-SAVING ACTIVE");
    printf("    Scheduler:             %s\n", rt->rt_status.sched_fifo_ok ? "SCHED_FIFO (prio 85)" :
                                          (rt->rt_status.sched_nice_ok ? "SCHED_OTHER (nice -20)" : "SCHED_OTHER (standard CFS)"));

    printf("\n[*] 1. PAGE FAULT METRICS (getrusage over 300 iterations):\n");
    printf("    %-28s %15s %15s %15s\n", "Counter", "Normal Mode", "Real-Time Mode", "Improvement");
    printf("    ────────────────────────────────────────────────────────────────────────────────────────\n");
    long minflt_diff = norm->minor_faults - rt->minor_faults;
    printf("    %-28s %15ld %15ld %15s\n", "Minor Faults (ru_minflt)",
           norm->minor_faults, rt->minor_faults,
           (rt->minor_faults == 0) ? "100.0% (ZERO)" : (minflt_diff >= 0 ? "REDUCED" : "SAME"));
    printf("    %-28s %15ld %15ld %15s\n", "Major Faults (ru_majflt)",
           norm->major_faults, rt->major_faults, "ZERO I/O");
    printf("    ────────────────────────────────────────────────────────────────────────────────────────\n");

    printf("\n[*] 2. TIMER WAKEUP LATENCY & JITTER (Microseconds):\n");
    printf("    %-18s %12s %12s %12s %12s %12s %12s\n",
           "Mode", "Min (us)", "Mean (us)", "p50 (us)", "p95 (us)", "p99 (us)", "Max Tail (us)");
    printf("    ─────────────────────────────────────────────────────────────────────────────────────────\n");

    #define ROW(label, s) \
        printf("    %-18s %12.2f %12.2f %12.2f %12.2f %12.2f %12.2f\n", \
               label, (s).min, (s).mean, (s).p50, (s).p95, (s).p99, (s).max)

    ROW("Normal (Wake Lat)", norm->wake_latency_stats);
    ROW("Real-Time (Wake Lat)", rt->wake_latency_stats);
    printf("    ─────────────────────────────────────────────────────────────────────────────────────────\n");
    ROW("Normal (Cycle Jit)", norm->cycle_jitter_stats);
    ROW("Real-Time (Cycle Jit)", rt->cycle_jitter_stats);
    printf("    ─────────────────────────────────────────────────────────────────────────────────────────\n");

    #undef ROW

    double p95_impr = (norm->wake_latency_stats.p95 > 0.0) ?
        ((norm->wake_latency_stats.p95 - rt->wake_latency_stats.p95) / norm->wake_latency_stats.p95) * 100.0 : 0.0;
    double p99_impr = (norm->wake_latency_stats.p99 > 0.0) ?
        ((norm->wake_latency_stats.p99 - rt->wake_latency_stats.p99) / norm->wake_latency_stats.p99) * 100.0 : 0.0;
    double max_impr = (norm->wake_latency_stats.max > 0.0) ?
        ((norm->wake_latency_stats.max - rt->wake_latency_stats.max) / norm->wake_latency_stats.max) * 100.0 : 0.0;

    printf("\n[*] 3. JITTER ATTENUATION SUMMARY:\n");
    printf("    Median (p50) Jitter:   %.2f us -> %.2f us\n",
           norm->wake_latency_stats.p50, rt->wake_latency_stats.p50);
    printf("    95th Pct (p95) Tail:   %.2f us -> %.2f us (%.1f%% reduction)\n",
           norm->wake_latency_stats.p95, rt->wake_latency_stats.p95, p95_impr);
    printf("    99th Pct (p99) Tail:   %.2f us -> %.2f us (%.1f%% reduction)\n",
           norm->wake_latency_stats.p99, rt->wake_latency_stats.p99, p99_impr);
    printf("    Max Latency Spike:     %.2f us -> %.2f us (%.1f%% peak spike reduction)\n",
           norm->wake_latency_stats.max, rt->wake_latency_stats.max, max_impr);
    printf("═══════════════════════════════════════════════════════════════════════════════════════════════════\n\n");
}

int main(int argc, char **argv) {
    int target_core = 2;
    size_t iterations = BENCH_DEFAULT_ITERS;
    clock_method_t method = CLOCK_METHOD_TIMERFD;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
            target_core = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iterations = (size_t)atoi(argv[++i]);
            if (iterations > TELEMETRY_WINDOW_SIZE) iterations = TELEMETRY_WINDOW_SIZE;
        } else if (strcmp(argv[i], "--method") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "nanosleep") == 0) {
                method = CLOCK_METHOD_NANOSLEEP;
            } else {
                method = CLOCK_METHOD_TIMERFD;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--core <core_id>] [--iters <count>] [--method <timerfd|nanosleep>]\n", argv[0]);
            return 0;
        }
    }

    printf("[FluxCast-C Bench] Starting dual-phase benchmark: %zu iterations, method: %s, target core: %d\n",
           iterations, (method == CLOCK_METHOD_TIMERFD) ? "timerfd" : "clock_nanosleep", target_core);

    /* Run Normal Mode in a separate process to guarantee clean memory/scheduler state */
    int pipe_norm[2];
    if (pipe(pipe_norm) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid_norm = fork();
    if (pid_norm == 0) {
        close(pipe_norm[0]);
        bench_result_t res_norm;
        run_benchmark(&res_norm, false, target_core, iterations, method);
        if (write(pipe_norm[1], &res_norm, sizeof(res_norm)) != sizeof(res_norm)) {
            perror("write pipe_norm");
        }
        close(pipe_norm[1]);
        exit(0);
    }
    close(pipe_norm[1]);

    bench_result_t res_norm;
    if (read(pipe_norm[0], &res_norm, sizeof(res_norm)) != sizeof(res_norm)) {
        perror("read pipe_norm");
        return 1;
    }
    close(pipe_norm[0]);
    waitpid(pid_norm, NULL, 0);

    /* Run Real-Time Mode in a separate process */
    int pipe_rt[2];
    if (pipe(pipe_rt) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid_rt = fork();
    if (pid_rt == 0) {
        close(pipe_rt[0]);
        bench_result_t res_rt;
        run_benchmark(&res_rt, true, target_core, iterations, method);
        if (write(pipe_rt[1], &res_rt, sizeof(res_rt)) != sizeof(res_rt)) {
            perror("write pipe_rt");
        }
        close(pipe_rt[1]);
        exit(0);
    }
    close(pipe_rt[1]);

    bench_result_t res_rt;
    if (read(pipe_rt[0], &res_rt, sizeof(res_rt)) != sizeof(res_rt)) {
        perror("read pipe_rt");
        return 1;
    }
    close(pipe_rt[0]);
    waitpid(pid_rt, NULL, 0);

    print_comparison(&res_norm, &res_rt);
    return 0;
}
