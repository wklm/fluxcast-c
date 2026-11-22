#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "realtime_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <math.h>

static int g_cpu_dma_latency_fd = -1;
static bool g_mlock_active = false;
static realtime_sys_status_t g_rt_status = {0};

/* Stack pre-faulting implementation */
void realtime_sys_prefault_stack(size_t bytes) {
    if (bytes == 0) {
        bytes = REALTIME_STACK_PREFAULT_BYTES;
    }
    volatile uint8_t stack_mem[bytes];
    for (size_t i = 0; i < bytes; i += 4096) {
        stack_mem[i] = 0;
    }
    stack_mem[bytes - 1] = 0;
    __asm__ __volatile__("" : : "r"(stack_mem) : "memory");
}

const realtime_sys_status_t *realtime_sys_get_status(void) {
    return &g_rt_status;
}

int realtime_sys_init(int target_cpu_core) {
    memset(&g_rt_status, 0, sizeof(g_rt_status));
    g_rt_status.target_cpu_core = target_cpu_core;
    g_rt_status.cpu_dma_latency_fd = -1;

    // 1. Lock process memory to eliminate page fault jitter
    struct rlimit rlim;
    if (getrlimit(RLIMIT_MEMLOCK, &rlim) == 0) {
        if (rlim.rlim_cur < rlim.rlim_max) {
            rlim.rlim_cur = rlim.rlim_max;
            setrlimit(RLIMIT_MEMLOCK, &rlim);
        }
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        g_rt_status.mlock_ok = true;
        g_mlock_active = true;
        printf("[FluxCast-C Realtime] mlockall(MCL_CURRENT | MCL_FUTURE) active - memory locked\n");
    } else {
        g_rt_status.mlock_ok = false;
        fprintf(stderr, "[FluxCast-C Realtime] Notice: mlockall failed: %s (errno %d)\n",
                strerror(errno), errno);
    }

    // 2. Pre-fault stack (64 KB)
    realtime_sys_prefault_stack(REALTIME_STACK_PREFAULT_BYTES);
    g_rt_status.prefault_ok = true;
    printf("[FluxCast-C Realtime] Pre-faulted %zu KB stack pages\n",
           (size_t)(REALTIME_STACK_PREFAULT_BYTES / 1024));

    // 3. Pin calling thread to target CPU core via pthread_setaffinity_np
    if (target_cpu_core >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu_core, &cpuset);
        int aff_err = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (aff_err == 0) {
            g_rt_status.affinity_ok = true;
            printf("[FluxCast-C Realtime] Pinned calling thread to CPU core %d\n", target_cpu_core);
        } else {
            g_rt_status.affinity_ok = false;
            fprintf(stderr, "[FluxCast-C Realtime] Warning: pthread_setaffinity_np(core %d) failed: %s\n",
                    target_cpu_core, strerror(aff_err));
        }
    }

    // 4. Open /dev/cpu_dma_latency and write 0 to lock CPU into C0/C1 states
    int fd = open("/dev/cpu_dma_latency", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        int32_t target_latency = 0;
        if (write(fd, &target_latency, sizeof(target_latency)) == sizeof(target_latency)) {
            g_rt_status.cpu_dma_latency_ok = true;
            g_cpu_dma_latency_fd = fd;
            g_rt_status.cpu_dma_latency_fd = fd;
            printf("[FluxCast-C Realtime] /dev/cpu_dma_latency locked to 0us (C-states suppressed)\n");
        } else {
            g_rt_status.cpu_dma_latency_ok = false;
            fprintf(stderr, "[FluxCast-C Realtime] Warning: write /dev/cpu_dma_latency failed: %s\n",
                    strerror(errno));
            close(fd);
            fd = -1;
        }
    } else {
        g_rt_status.cpu_dma_latency_ok = false;
        printf("[FluxCast-C Realtime] Notice: /dev/cpu_dma_latency unavailable: %s (requires root/CAP_SYS_ADMIN)\n",
               strerror(errno));
    }

    // 5. Attempt sched_setscheduler(SCHED_FIFO, 85); fallback to setpriority -20
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 85;

    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        g_rt_status.sched_fifo_ok = true;
        printf("[FluxCast-C Realtime] Set SCHED_FIFO priority 85 successfully\n");
    } else {
        int fifo_err = errno;
        g_rt_status.sched_fifo_ok = false;
        if (setpriority(PRIO_PROCESS, 0, -20) == 0) {
            g_rt_status.sched_nice_ok = true;
            printf("[FluxCast-C Realtime] SCHED_FIFO unavailable (%s), fallback to setpriority(PRIO_PROCESS, -20) succeeded\n",
                   strerror(fifo_err));
        } else {
            g_rt_status.sched_nice_ok = false;
            printf("[FluxCast-C Realtime] Notice: SCHED_FIFO (%s) and setpriority -20 (%s) unavailable. Standard priority retained.\n",
                   strerror(fifo_err), strerror(errno));
        }
    }

    return 0;
}

void realtime_sys_teardown(void) {
    if (g_cpu_dma_latency_fd >= 0) {
        close(g_cpu_dma_latency_fd);
        g_cpu_dma_latency_fd = -1;
        printf("[FluxCast-C Realtime] Closed /dev/cpu_dma_latency\n");
    }
    if (g_mlock_active) {
        munlockall();
        g_mlock_active = false;
    }
}

/* ========================================================================= */
/* Telemetry Implementation                                                  */
/* ========================================================================= */

void telemetry_init(pipeline_telemetry_t *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
}

void telemetry_record_frame(pipeline_telemetry_t *t,
                            double t_capture_us,
                            double t_convert_us,
                            double t_upload_us,
                            double t_encode_us,
                            double t_pacing_us,
                            double t_total_us) {
    if (!t) return;

    t->t_capture_us = t_capture_us;
    t->t_convert_us = t_convert_us;
    t->t_upload_us  = t_upload_us;
    t->t_encode_us  = t_encode_us;
    t->t_pacing_us  = t_pacing_us;
    t->t_total_us   = t_total_us;

    size_t idx = t->index;
    t->hist_capture[idx] = t_capture_us;
    t->hist_convert[idx] = t_convert_us;
    t->hist_upload[idx]  = t_upload_us;
    t->hist_encode[idx]  = t_encode_us;
    t->hist_pacing[idx]  = t_pacing_us;
    t->hist_total[idx]   = t_total_us;

    t->index = (idx + 1) % TELEMETRY_WINDOW_SIZE;
    t->count++;
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static double compute_percentile(const double *sorted, size_t n, double p) {
    if (n == 0) return 0.0;
    if (n == 1) return sorted[0];
    double rank = (p / 100.0) * (n - 1);
    size_t idx_low = (size_t)rank;
    size_t idx_high = (idx_low + 1 < n) ? idx_low + 1 : idx_low;
    double weight = rank - (double)idx_low;
    return sorted[idx_low] * (1.0 - weight) + sorted[idx_high] * weight;
}

void telemetry_compute_stage_stats(const double *samples, size_t count, latency_stats_t *out_stats) {
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));

    if (count == 0 || !samples) {
        return;
    }

    double sorted[TELEMETRY_WINDOW_SIZE];
    size_t n = count > TELEMETRY_WINDOW_SIZE ? TELEMETRY_WINDOW_SIZE : count;
    double sum = 0.0;

    for (size_t i = 0; i < n; i++) {
        sorted[i] = samples[i];
        sum += samples[i];
    }

    qsort(sorted, n, sizeof(double), compare_doubles);

    out_stats->min  = sorted[0];
    out_stats->max  = sorted[n - 1];
    out_stats->mean = sum / (double)n;
    out_stats->p50  = compute_percentile(sorted, n, 50.0);
    out_stats->p95  = compute_percentile(sorted, n, 95.0);
    out_stats->p99  = compute_percentile(sorted, n, 99.0);
}

void telemetry_compute_stats(const pipeline_telemetry_t *t, pipeline_stats_t *out_stats) {
    if (!t || !out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));

    size_t n = (t->count < TELEMETRY_WINDOW_SIZE) ? t->count : TELEMETRY_WINDOW_SIZE;
    if (n == 0) return;

    telemetry_compute_stage_stats(t->hist_capture, n, &out_stats->capture);
    telemetry_compute_stage_stats(t->hist_convert, n, &out_stats->convert);
    telemetry_compute_stage_stats(t->hist_upload,  n, &out_stats->upload);
    telemetry_compute_stage_stats(t->hist_encode,  n, &out_stats->encode);
    telemetry_compute_stage_stats(t->hist_pacing,  n, &out_stats->pacing);
    telemetry_compute_stage_stats(t->hist_total,   n, &out_stats->total);
}

void telemetry_print_summary(const pipeline_telemetry_t *t, const char *prefix) {
    if (!t) return;

    pipeline_stats_t stats;
    telemetry_compute_stats(t, &stats);
    size_t n = (t->count < TELEMETRY_WINDOW_SIZE) ? t->count : TELEMETRY_WINDOW_SIZE;

    const char *p = prefix ? prefix : "[FluxCast-C Telemetry]";
    printf("%s ── Window: %zu frames (Total: %zu frames) ────────────────────────\n",
           p, n, t->count);
    printf("%-16s %10s %10s %10s %10s %10s %10s\n",
           "Stage", "Min (us)", "Mean (us)", "p50 (us)", "p95 (us)", "p99 (us)", "Max (us)");
    printf("──────────────────────────────────────────────────────────────────────────────────\n");

    #define ROW(name, s) \
        printf("%-16s %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f\n", \
               name, (s).min, (s).mean, (s).p50, (s).p95, (s).p99, (s).max)

    ROW("t_capture_us", stats.capture);
    ROW("t_convert_us", stats.convert);
    ROW("t_upload_us",  stats.upload);
    ROW("t_encode_us",  stats.encode);
    ROW("t_pacing_us",  stats.pacing);
    ROW("t_total_us",   stats.total);

    #undef ROW
    printf("──────────────────────────────────────────────────────────────────────────────────\n");
}
