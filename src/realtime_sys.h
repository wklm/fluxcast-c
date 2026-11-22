#ifndef REALTIME_SYS_H
#define REALTIME_SYS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELEMETRY_WINDOW_SIZE 300
#define REALTIME_STACK_PREFAULT_BYTES (64 * 1024)

/* Single metric latency statistics calculated over rolling window */
typedef struct {
    double min;
    double mean;
    double p50;   /* Median */
    double p95;   /* 95th percentile */
    double p99;   /* 99th percentile */
    double max;   /* Tail latency */
} latency_stats_t;

/* Aggregated pipeline statistics across all stages */
typedef struct {
    latency_stats_t capture;
    latency_stats_t convert;
    latency_stats_t upload;
    latency_stats_t encode;
    latency_stats_t pacing;
    latency_stats_t total;
} pipeline_stats_t;

/* Struct tracking per-frame stages and maintaining 300-frame rolling window */
typedef struct pipeline_telemetry {
    /* Per-frame timing measurements (in microseconds) */
    double t_capture_us;
    double t_convert_us;
    double t_upload_us;
    double t_encode_us;
    double t_pacing_us;
    double t_total_us;

    /* Rolling window circular buffer of 300 frames */
    double hist_capture[TELEMETRY_WINDOW_SIZE];
    double hist_convert[TELEMETRY_WINDOW_SIZE];
    double hist_upload[TELEMETRY_WINDOW_SIZE];
    double hist_encode[TELEMETRY_WINDOW_SIZE];
    double hist_pacing[TELEMETRY_WINDOW_SIZE];
    double hist_total[TELEMETRY_WINDOW_SIZE];

    size_t count;  /* Total frames recorded */
    size_t index;  /* Circular ring buffer write index */
} pipeline_telemetry_t;

/* Status tracking for realtime subsystem operations */
typedef struct {
    bool mlock_ok;
    bool prefault_ok;
    bool affinity_ok;
    bool cpu_dma_latency_ok;
    bool sched_fifo_ok;
    bool sched_nice_ok;
    int target_cpu_core;
    int cpu_dma_latency_fd;
} realtime_sys_status_t;

/* Real-time system initialization and cleanup */
int realtime_sys_init(int target_cpu_core);
void realtime_sys_teardown(void);
const realtime_sys_status_t *realtime_sys_get_status(void);

/* Stack pre-faulting utility */
void realtime_sys_prefault_stack(size_t bytes);

/* Monotonic microsecond timestamp helpers */
static inline struct timespec realtime_sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

static inline double realtime_sys_elapsed_us(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) * 1000000.0 +
           (double)(end->tv_nsec - start->tv_nsec) / 1000.0;
}

/* Telemetry API */
void telemetry_init(pipeline_telemetry_t *t);
void telemetry_record_frame(pipeline_telemetry_t *t,
                            double t_capture_us,
                            double t_convert_us,
                            double t_upload_us,
                            double t_encode_us,
                            double t_pacing_us,
                            double t_total_us);

void telemetry_compute_stage_stats(const double *samples, size_t count, latency_stats_t *out_stats);
void telemetry_compute_stats(const pipeline_telemetry_t *t, pipeline_stats_t *out_stats);
void telemetry_print_summary(const pipeline_telemetry_t *t, const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* REALTIME_SYS_H */
