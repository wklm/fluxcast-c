#ifndef NET_PACER_H
#define NET_PACER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 802.11 Wi-Fi Multimedia (WMM) Voice & Microsecond Packet Pacing Configuration
 *
 * Latency & Contention Specifications:
 * - 60 Hz frame interval = 16.666 ms (16,666,667 ns).
 * - 80% duty cycle transmission budget = 13.333 ms (13,333,333 ns).
 * - Leaves 3.333 ms channel silence before next frame VSYNC.
 * - SO_PRIORITY = 6: Maps to AC_VO (Voice) in mac80211, clamping CW_max=7 and contention to <0.1 ms.
 * - IP_TOS = 0xC0: DSCP CS6 (Class Selector 6 / Network Control / Voice).
 * - SO_SNDBUF = 65536: prevents bufferbloat (clamping buffering to < 25ms).
 * - Uses absolute CLOCK_MONOTONIC deadlines and reports departure/queue telemetry.
 */

#define NET_PACER_DEFAULT_FPS               60
#define NET_PACER_FRAME_BUDGET_NS           16666667ULL  /* 16.666 ms total frame period */
#define NET_PACER_DUTY_CYCLE_PERCENT        80
#define NET_PACER_DEFAULT_PACING_BUDGET_NS  13333333ULL  /* 13.333 ms (80% of 16.666 ms) */

#define NET_PACER_SO_PRIORITY               6            /* WMM Voice AC_VO */
#define NET_PACER_IP_TOS                    0xC0         /* DSCP CS6 (Voice / Network Control) */
#define NET_PACER_SO_SNDBUF                 65536        /* 64 KB requested; prevents bufferbloat (< 25ms) */

#define NET_PACER_MAX_PACKET_SIZE           1500         /* Maximum Ethernet/UDP packet MTU */
#define NET_PACER_MAX_FRAME_PACKETS         1024         /* Up to ~1.4 MB per frame */
#define NET_PACER_RTP_PACKET_SIZE           1328         /* Standard Miracast: 12B RTP + 7*188B TS */
#define NET_PACER_TS_PACKET_SIZE            188
#define NET_PACER_TS_PER_RTP                7

/* Configuration struct for initializing net_pacer */
typedef struct {
    const char *local_ip;          /* Source IP (e.g. "192.168.179.200", NULL for INADDR_ANY) */
    uint16_t    local_port;        /* Source UDP port (e.g. 19002, 0 for auto-assign) */
    const char *dest_ip;           /* Destination IP (e.g. "192.168.179.1") */
    uint16_t    dest_port;         /* Destination UDP port (e.g. 1010 or 19002) */
    uint64_t    pacing_budget_ns;  /* Budget across which to pace packets (default 13333333 ns) */
    bool        enable_pacing;     /* true: microsecond pacing; false: unpaced burst */
} net_pacer_cfg_t;

/* Preallocated packet container in frame accumulation queue */
typedef struct {
    uint8_t data[NET_PACER_MAX_PACKET_SIZE];
    size_t  len;
} net_pacer_packet_t;

/* Per-packet telemetry entry for benchmarking and precision jitter verification */
typedef struct {
    uint64_t target_ns;         /* Ideal scheduled departure timestamp (CLOCK_MONOTONIC ns) */
    uint64_t departure_ns;      /* Actual post-send departure timestamp (CLOCK_MONOTONIC ns) */
    int64_t  jitter_ns;         /* Timing error / deviation: (departure_ns - target_ns) */
    int32_t  tx_queue_bytes;    /* Socket TX queue depth (SIOCOUTQ) prior to send */
} net_pacer_telemetry_t;

/* Runtime cumulative and last-frame statistics */
typedef struct {
    uint64_t frames_sent;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t send_errors;
    uint32_t last_frame_packets;
    uint64_t last_frame_pace_interval_ns;
    uint64_t last_frame_total_duration_ns;
    int32_t  peak_tx_queue_bytes;
} net_pacer_stats_t;

/* Network pacer state structure */
typedef struct {
    int                sockfd;
    struct sockaddr_in local_addr;
    struct sockaddr_in dest_addr;
    bool               is_connected;
    net_pacer_cfg_t    cfg;

    /* Frame packet accumulation pool */
    net_pacer_packet_t packets[NET_PACER_MAX_FRAME_PACKETS];
    uint32_t           num_packets;

    /* Cumulative metrics */
    net_pacer_stats_t  stats;
} net_pacer_t;

/**
 * Initialize net_pacer:
 * - Creates UDP socket (AF_INET, SOCK_DGRAM).
 * - Applies WMM Voice QoS (SO_PRIORITY = 6, IP_TOS = 0xC0).
 * - Sets SO_SNDBUF = 65536 to prevent bufferbloat.
 * - Binds to local_ip:local_port.
 * - Configures destination endpoint and connects socket.
 *
 * Returns 0 on success, negative error code on failure.
 */
int  net_pacer_init(net_pacer_t *pacer, const net_pacer_cfg_t *cfg);

/**
 * Close socket and release net_pacer resources.
 */
void net_pacer_close(net_pacer_t *pacer);

/**
 * Returns underlying socket file descriptor.
 */
int  net_pacer_get_fd(const net_pacer_t *pacer);

/**
 * Toggle packet pacing on or off dynamically.
 */
void net_pacer_set_pacing(net_pacer_t *pacer, bool enable_pacing);

/**
 * Update pacing budget in nanoseconds (default is 13,333,333 ns for 80% of 60Hz).
 */
void net_pacer_set_budget_ns(net_pacer_t *pacer, uint64_t pacing_budget_ns);

/**
 * Update destination endpoint (IP and port).
 */
int  net_pacer_set_destination(net_pacer_t *pacer, const char *dest_ip, uint16_t dest_port);

/**
 * Begin a new frame accumulation cycle by clearing the packet buffer.
 */
int  net_pacer_frame_begin(net_pacer_t *pacer);

/**
 * Queue an RTP packet into the current frame buffer.
 * Copies data into the internal zero-heap pool.
 *
 * Returns 0 on success, -ENOMEM if frame buffer full, -EINVAL if packet exceeds MTU.
 */
int  net_pacer_queue_packet(net_pacer_t *pacer, const void *data, size_t len);

/**
 * Dispatches all accumulated packets for the current frame, using the dispatch
 * timestamp rather than the earlier capture/encode start as the pacing origin:
 * - If pacing enabled: spreads N packets evenly across pacing_budget_ns (13.33 ms)
 *   with delta_t = pacing_budget_ns / N using clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...).
 * - If pacing disabled: burst sends all packets immediately back-to-back.
 * - Reports per-packet timing and socket queue depth through telemetry.
 *
 * Returns number of packets successfully dispatched, or negative error code.
 */
int  net_pacer_send_frame(net_pacer_t *pacer);

/**
 * Dispatches frame packets with microsecond telemetry collection per packet.
 * Writes target departure, actual departure, jitter, and queue depth into telemetry_out.
 *
 * Returns number of packets successfully dispatched.
 */
int  net_pacer_send_frame_measured(net_pacer_t *pacer,
                                   net_pacer_telemetry_t *telemetry_out,
                                   size_t max_telemetry);

/**
 * Sends a single packet immediately bypassing frame queue.
 */
int  net_pacer_send_packet_raw(net_pacer_t *pacer, const void *data, size_t len);

/**
 * Query current socket TX queue depth in bytes via SIOCOUTQ.
 */
int  net_pacer_get_tx_queue_bytes(const net_pacer_t *pacer);

/**
 * Copy cumulative statistics to out_stats.
 */
void net_pacer_get_stats(const net_pacer_t *pacer, net_pacer_stats_t *out_stats);

/**
 * Reset cumulative statistics counters.
 */
void net_pacer_reset_stats(net_pacer_t *pacer);

#ifdef __cplusplus
}
#endif

#endif /* NET_PACER_H */
