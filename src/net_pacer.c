#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "net_pacer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <arpa/inet.h>

#ifndef SIOCOUTQ
#define SIOCOUTQ TIOCOUTQ
#endif

int net_pacer_init(net_pacer_t *pacer, const net_pacer_cfg_t *cfg) {
    if (!pacer || !cfg) {
        return -EINVAL;
    }

    memset(pacer, 0, sizeof(*pacer));
    pacer->cfg = *cfg;
    if (pacer->cfg.pacing_budget_ns == 0) {
        pacer->cfg.pacing_budget_ns = NET_PACER_DEFAULT_PACING_BUDGET_NS;
    }

    // 1. Create UDP socket
    pacer->sockfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (pacer->sockfd < 0) {
        int err = -errno;
        fprintf(stderr, "[FluxCast-C NetPacer] Failed to create UDP socket: %s\n", strerror(errno));
        return err;
    }

    // 3. Set IP_TOS = 0xC0: DSCP CS6 (Class Selector 6 / Network Control / Voice)
    // NOTE: In the Linux kernel (ip_sockglue.c), setsockopt(IP_TOS) automatically resets
    // sk->sk_priority = rt_tos2priority(tos). Therefore, IP_TOS MUST be configured BEFORE
    // SO_PRIORITY so that our explicit AC_VO priority overrides the default kernel mapping!
    int tos = NET_PACER_IP_TOS;
    if (setsockopt(pacer->sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
        fprintf(stderr, "[FluxCast-C NetPacer] Warning: Failed to set IP_TOS=0x%02X: %s\n",
                tos, strerror(errno));
    }

    // 4. Apply Wi-Fi Multimedia (WMM) Voice QoS: SO_PRIORITY = 6
    // User Priority 6 maps directly to Access Category Voice (AC_VO) in mac80211.
    // In 802.11 EDCA, AC_VO clamps CW_min=3, CW_max=7, AIFS=2 slots, ensuring contention delay < 0.1 ms.
    int prio = NET_PACER_SO_PRIORITY;
    if (setsockopt(pacer->sockfd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0) {
        fprintf(stderr, "[FluxCast-C NetPacer] Warning: Failed to set SO_PRIORITY=%d: %s\n",
                prio, strerror(errno));
    }

    // 5. Set SO_SNDBUF = 65536 to prevent bufferbloat (< 25ms buffering).
    // Capacity does not set the departure rate; the pacer still does.
    int sndbuf = NET_PACER_SO_SNDBUF;
    if (setsockopt(pacer->sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        fprintf(stderr, "[FluxCast-C NetPacer] Warning: Failed to set SO_SNDBUF=%d: %s\n",
                sndbuf, strerror(errno));
    }

    // Verify QoS options via getsockopt
    int read_prio = 0, read_tos = 0, read_sndbuf = 0;
    socklen_t optlen = sizeof(int);
    optlen = sizeof(int);
    getsockopt(pacer->sockfd, SOL_SOCKET, SO_PRIORITY, &read_prio, &optlen);
    optlen = sizeof(int);
    getsockopt(pacer->sockfd, IPPROTO_IP, IP_TOS, &read_tos, &optlen);
    optlen = sizeof(int);
    getsockopt(pacer->sockfd, SOL_SOCKET, SO_SNDBUF, &read_sndbuf, &optlen);

    // 6. Bind to local_ip:local_port
    pacer->local_addr.sin_family = AF_INET;
    pacer->local_addr.sin_port = htons(cfg->local_port);
    if (cfg->local_ip && strlen(cfg->local_ip) > 0 && strcmp(cfg->local_ip, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, cfg->local_ip, &pacer->local_addr.sin_addr) <= 0) {
            fprintf(stderr, "[FluxCast-C NetPacer] Invalid local IP: %s\n", cfg->local_ip);
            close(pacer->sockfd);
            pacer->sockfd = -1;
            return -EINVAL;
        }
    } else {
        pacer->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(pacer->sockfd, (struct sockaddr *)&pacer->local_addr, sizeof(pacer->local_addr)) < 0) {
        int err = -errno;
        fprintf(stderr, "[FluxCast-C NetPacer] Failed to bind local socket %s:%d: %s\n",
                cfg->local_ip ? cfg->local_ip : "0.0.0.0", cfg->local_port, strerror(errno));
        close(pacer->sockfd);
        pacer->sockfd = -1;
        return err;
    }

    // Refresh actual bound local port if dynamic port was requested
    socklen_t local_len = sizeof(pacer->local_addr);
    getsockname(pacer->sockfd, (struct sockaddr *)&pacer->local_addr, &local_len);

    // 7. Configure destination endpoint if provided
    if (cfg->dest_ip && strlen(cfg->dest_ip) > 0 && cfg->dest_port > 0) {
        pacer->dest_addr.sin_family = AF_INET;
        pacer->dest_addr.sin_port = htons(cfg->dest_port);
        if (inet_pton(AF_INET, cfg->dest_ip, &pacer->dest_addr.sin_addr) <= 0) {
            fprintf(stderr, "[FluxCast-C NetPacer] Invalid dest IP: %s\n", cfg->dest_ip);
            close(pacer->sockfd);
            pacer->sockfd = -1;
            return -EINVAL;
        }

        // Connecting the UDP socket caches the routing decision in kernel and enables send()
        if (connect(pacer->sockfd, (struct sockaddr *)&pacer->dest_addr, sizeof(pacer->dest_addr)) == 0) {
            pacer->is_connected = true;
        } else {
            pacer->is_connected = false;
        }
    }

    char local_ip_str[INET_ADDRSTRLEN];
    char dest_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &pacer->local_addr.sin_addr, local_ip_str, sizeof(local_ip_str));
    inet_ntop(AF_INET, &pacer->dest_addr.sin_addr, dest_ip_str, sizeof(dest_ip_str));

    printf("[FluxCast-C NetPacer] Initialized RTP UDP socket fd=%d\n"
           "  Local Endpoint:  %s:%d\n"
           "  Dest Endpoint:   %s:%d (%s)\n"
           "  WMM Voice QoS:   SO_PRIORITY=%d (AC_VO), IP_TOS=0x%02X (DSCP CS6)\n"
           "  Buffer & Budget: SO_SNDBUF=%d, Pacing Budget=%.2f ms (%s)\n",
           pacer->sockfd,
           local_ip_str, ntohs(pacer->local_addr.sin_port),
           dest_ip_str, ntohs(pacer->dest_addr.sin_port),
           pacer->is_connected ? "connected" : "unconnected",
           read_prio, read_tos,
           read_sndbuf,
           (double)pacer->cfg.pacing_budget_ns / 1000000.0,
           pacer->cfg.enable_pacing ? "Microsecond Pacing Active" : "Unpaced Burst Mode");

    return 0;
}

void net_pacer_close(net_pacer_t *pacer) {
    if (!pacer) return;
    if (pacer->sockfd >= 0) {
        close(pacer->sockfd);
        pacer->sockfd = -1;
    }
    pacer->num_packets = 0;
    pacer->is_connected = false;
}

int net_pacer_get_fd(const net_pacer_t *pacer) {
    return pacer ? pacer->sockfd : -1;
}

void net_pacer_set_pacing(net_pacer_t *pacer, bool enable_pacing) {
    if (pacer) {
        pacer->cfg.enable_pacing = enable_pacing;
    }
}

void net_pacer_set_budget_ns(net_pacer_t *pacer, uint64_t pacing_budget_ns) {
    if (pacer && pacing_budget_ns > 0) {
        pacer->cfg.pacing_budget_ns = pacing_budget_ns;
    }
}

int net_pacer_set_destination(net_pacer_t *pacer, const char *dest_ip, uint16_t dest_port) {
    if (!pacer || !dest_ip || dest_port == 0) return -EINVAL;

    pacer->dest_addr.sin_family = AF_INET;
    pacer->dest_addr.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &pacer->dest_addr.sin_addr) <= 0) {
        return -EINVAL;
    }

    if (pacer->sockfd >= 0) {
        if (connect(pacer->sockfd, (struct sockaddr *)&pacer->dest_addr, sizeof(pacer->dest_addr)) == 0) {
            pacer->is_connected = true;
        } else {
            pacer->is_connected = false;
        }
    }
    return 0;
}

int net_pacer_frame_begin(net_pacer_t *pacer) {
    if (!pacer) return -EINVAL;
    pacer->num_packets = 0;
    return 0;
}

int net_pacer_queue_packet(net_pacer_t *pacer, const void *data, size_t len) {
    if (!pacer || !data) return -EINVAL;
    if (len > NET_PACER_MAX_PACKET_SIZE) return -EMSGSIZE;
    if (pacer->num_packets >= NET_PACER_MAX_FRAME_PACKETS) return -ENOMEM;

    memcpy(pacer->packets[pacer->num_packets].data, data, len);
    pacer->packets[pacer->num_packets].len = len;
    pacer->num_packets++;

    return 0;
}

int net_pacer_send_frame(net_pacer_t *pacer) {
    return net_pacer_send_frame_measured(pacer, NULL, 0);
}

int net_pacer_send_frame_measured(net_pacer_t *pacer,
                                   net_pacer_telemetry_t *telemetry_out,
                                   size_t max_telemetry) {
    if (!pacer || pacer->sockfd < 0) return -EINVAL;
    uint32_t n = pacer->num_packets;
    if (n == 0) return 0;

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t t_start_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + ts_now.tv_nsec;

    uint64_t budget_ns = pacer->cfg.pacing_budget_ns ? pacer->cfg.pacing_budget_ns : NET_PACER_DEFAULT_PACING_BUDGET_NS;
    // Spread packet departures evenly across the current frame's budget.
    uint64_t delta_ns = (n > 1) ? (budget_ns / n) : budget_ns;

    int sent_count = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t target_ns = t_start_ns + (uint64_t)i * delta_ns;

        if (pacer->cfg.enable_pacing) {
            struct timespec next_send;
            next_send.tv_sec  = (time_t)(target_ns / 1000000000ULL);
            next_send.tv_nsec = (long)(target_ns % 1000000000ULL);

            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_send, NULL) == EINTR) {
                // Interrupted by signal, retry until next_send
            }
        }

        bool collect_telemetry = telemetry_out && (size_t)i < max_telemetry;
        int q_bytes = 0;
        if (collect_telemetry && ioctl(pacer->sockfd, SIOCOUTQ, &q_bytes) == 0) {
            if (q_bytes > pacer->stats.peak_tx_queue_bytes) {
                pacer->stats.peak_tx_queue_bytes = q_bytes;
            }
        }

        // Transmit packet
        ssize_t bytes_sent;
        if (pacer->is_connected) {
            bytes_sent = send(pacer->sockfd, pacer->packets[i].data, pacer->packets[i].len, 0);
        } else {
            bytes_sent = sendto(pacer->sockfd, pacer->packets[i].data, pacer->packets[i].len, 0,
                                (struct sockaddr *)&pacer->dest_addr, sizeof(pacer->dest_addr));
        }

        if (bytes_sent >= 0) {
            sent_count++;
            pacer->stats.packets_sent++;
            pacer->stats.bytes_sent += (uint64_t)bytes_sent;
        } else {
            pacer->stats.send_errors++;
        }

        if (collect_telemetry) {
            struct timespec ts_dep;
            clock_gettime(CLOCK_MONOTONIC, &ts_dep);
            uint64_t departure_ns = (uint64_t)ts_dep.tv_sec * 1000000000ULL + ts_dep.tv_nsec;
            telemetry_out[i].target_ns      = target_ns;
            telemetry_out[i].departure_ns   = departure_ns;
            telemetry_out[i].jitter_ns      = (int64_t)departure_ns - (int64_t)target_ns;
            telemetry_out[i].tx_queue_bytes = q_bytes;
        }
    }

    pacer->stats.frames_sent++;
    pacer->stats.last_frame_packets = n;
    pacer->stats.last_frame_pace_interval_ns = delta_ns;

    struct timespec ts_finish;
    clock_gettime(CLOCK_MONOTONIC, &ts_finish);
    uint64_t finish_ns = (uint64_t)ts_finish.tv_sec * 1000000000ULL + ts_finish.tv_nsec;
    pacer->stats.last_frame_total_duration_ns = finish_ns - t_start_ns;

    pacer->num_packets = 0;

    return sent_count;
}

int net_pacer_send_packet_raw(net_pacer_t *pacer, const void *data, size_t len) {
    if (!pacer || pacer->sockfd < 0 || !data || len == 0) return -EINVAL;

    ssize_t ret;
    if (pacer->is_connected) {
        ret = send(pacer->sockfd, data, len, 0);
    } else {
        ret = sendto(pacer->sockfd, data, len, 0,
                     (struct sockaddr *)&pacer->dest_addr, sizeof(pacer->dest_addr));
    }

    if (ret >= 0) {
        pacer->stats.packets_sent++;
        pacer->stats.bytes_sent += (uint64_t)ret;
        return (int)ret;
    } else {
        pacer->stats.send_errors++;
        return -errno;
    }
}

int net_pacer_get_tx_queue_bytes(const net_pacer_t *pacer) {
    if (!pacer || pacer->sockfd < 0) return -EINVAL;
    int q_bytes = 0;
    if (ioctl(pacer->sockfd, SIOCOUTQ, &q_bytes) < 0) {
        return -errno;
    }
    return q_bytes;
}

void net_pacer_get_stats(const net_pacer_t *pacer, net_pacer_stats_t *out_stats) {
    if (pacer && out_stats) {
        *out_stats = pacer->stats;
    }
}

void net_pacer_reset_stats(net_pacer_t *pacer) {
    if (pacer) {
        memset(&pacer->stats, 0, sizeof(pacer->stats));
    }
}
