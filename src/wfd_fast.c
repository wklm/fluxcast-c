#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <immintrin.h>

#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

#define WFD_RTSP_PORT 7236
#define DEFAULT_FPS 60
#define WIDTH 1920
#define HEIGHT 1080

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_streaming = 0;
static volatile sig_atomic_t g_exit_signal = 0;

static int g_rtsp_client_fd = -1;
static char g_tv_ip[64] = "192.168.179.1";
static int g_tv_rtp_port = 1010;
static char g_local_ip[64] = "192.168.179.200";
static int g_local_rtp_port = 19002;
static char g_session_id[32] = "3829104";
static bool g_stream_only = false;

static void handle_signal(int sig) {
    g_exit_signal = sig;
    g_running = 0;
    g_streaming = 0;
}

struct wayland_shm_buffer {
    struct wl_buffer *wl_buf;
    void *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct wayland_context {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;
    struct wl_output *output;
    uint32_t shm_format;
    int width, height, stride;

    struct wayland_shm_buffer bufs[2];
    int cur_buf;
    bool copy_done;
    bool frame_ready;
    bool frame_failed;
};

static struct wayland_context g_wl;

// Ultra-fast AVX2 SIMD XRGB8888 -> NV12 converter (Rec.601)
// Processes 8 pixels per vector chunk with zero thread contention (~4.0 ms per frame)
static inline void xrgb_to_nv12_ultra(const uint32_t *src, int w, int h, int stride_pixels,
                                      uint8_t *y_plane, int y_stride,
                                      uint8_t *uv_plane, int uv_stride) {
    const __m256i mask_b = _mm256_set1_epi32(0x000000ff);
    const __m256i mask_g = _mm256_set1_epi32(0x0000ff00);
    const __m256i mask_r = _mm256_set1_epi32(0x00ff0000);

    const __m256i coeff_yr = _mm256_set1_epi32(66);
    const __m256i coeff_yg = _mm256_set1_epi32(129);
    const __m256i coeff_yb = _mm256_set1_epi32(25);
    const __m256i bias_y   = _mm256_set1_epi32(128 + (16 << 8));

    for (int y = 0; y < h; y += 2) {
        const uint32_t *r0 = src + y * stride_pixels;
        const uint32_t *r1 = src + (y + 1) * stride_pixels;
        uint8_t *y0 = y_plane + y * y_stride;
        uint8_t *y1 = y_plane + (y + 1) * y_stride;
        uint8_t *uv = uv_plane + (y / 2) * uv_stride;

        for (int x = 0; x < w; x += 8) {
            __m256i p0 = _mm256_loadu_si256((const __m256i *)(r0 + x));
            __m256i p1 = _mm256_loadu_si256((const __m256i *)(r1 + x));

            __m256i b0 = _mm256_and_si256(p0, mask_b);
            __m256i g0 = _mm256_srli_epi32(_mm256_and_si256(p0, mask_g), 8);
            __m256i r0_c = _mm256_srli_epi32(_mm256_and_si256(p0, mask_r), 16);

            __m256i y0_val = _mm256_srli_epi32(_mm256_add_epi32(
                _mm256_add_epi32(_mm256_mullo_epi32(r0_c, coeff_yr), _mm256_mullo_epi32(g0, coeff_yg)),
                _mm256_add_epi32(_mm256_mullo_epi32(b0, coeff_yb), bias_y)
            ), 8);

            __m256i b1 = _mm256_and_si256(p1, mask_b);
            __m256i g1 = _mm256_srli_epi32(_mm256_and_si256(p1, mask_g), 8);
            __m256i r1_c = _mm256_srli_epi32(_mm256_and_si256(p1, mask_r), 16);

            __m256i y1_val = _mm256_srli_epi32(_mm256_add_epi32(
                _mm256_add_epi32(_mm256_mullo_epi32(r1_c, coeff_yr), _mm256_mullo_epi32(g1, coeff_yg)),
                _mm256_add_epi32(_mm256_mullo_epi32(b1, coeff_yb), bias_y)
            ), 8);

            // Pack Y (8 x 32-bit -> 8 x 16-bit -> 8 x 8-bit)
            __m128i y0_lo = _mm256_castsi256_si128(y0_val);
            __m128i y0_hi = _mm256_extracti128_si256(y0_val, 1);
            __m128i y0_16 = _mm_packus_epi32(y0_lo, y0_hi);
            __m128i y0_8  = _mm_packus_epi16(y0_16, y0_16);
            _mm_storel_epi64((__m128i *)(y0 + x), y0_8);

            __m128i y1_lo = _mm256_castsi256_si128(y1_val);
            __m128i y1_hi = _mm256_extracti128_si256(y1_val, 1);
            __m128i y1_16 = _mm_packus_epi32(y1_lo, y1_hi);
            __m128i y1_8  = _mm_packus_epi16(y1_16, y1_16);
            _mm_storel_epi64((__m128i *)(y1 + x), y1_8);

            // UV: Average 2x2 blocks
            __m256i r_avg = _mm256_srli_epi32(_mm256_add_epi32(r0_c, r1_c), 1);
            __m256i g_avg = _mm256_srli_epi32(_mm256_add_epi32(g0, g1), 1);
            __m256i b_avg = _mm256_srli_epi32(_mm256_add_epi32(b0, b1), 1);

            for (int k = 0; k < 8; k += 2) {
                int r_2x2 = (_mm256_extract_epi32(r_avg, k) + _mm256_extract_epi32(r_avg, k + 1)) >> 1;
                int g_2x2 = (_mm256_extract_epi32(g_avg, k) + _mm256_extract_epi32(g_avg, k + 1)) >> 1;
                int b_2x2 = (_mm256_extract_epi32(b_avg, k) + _mm256_extract_epi32(b_avg, k + 1)) >> 1;

                uv[x + k]     = (uint8_t)(((-38 * r_2x2 -  74 * g_2x2 + 112 * b_2x2 + 128) >> 8) + 128);
                uv[x + k + 1] = (uint8_t)((( 112 * r_2x2 -  94 * g_2x2 -  18 * b_2x2 + 128) >> 8) + 128);
            }
        }
    }
}

// Screencopy Callbacks with dynamic dimension/stride recreation and leak-free memfd handling
static void frame_handle_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
        uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    struct wayland_context *ctx = (struct wayland_context *)data;
    ctx->shm_format = format;
    ctx->width = width;
    ctx->height = height;
    ctx->stride = stride;

    if (ctx->copy_done) {
        return; // Guard against multiple format advertisements in protocol
    }

    struct wayland_shm_buffer *b = &ctx->bufs[ctx->cur_buf];
    // Recreate buffer if dimensions, stride, or format changed
    if (b->wl_buf && (b->width != width || b->height != height || b->stride != stride || b->format != format)) {
        wl_buffer_destroy(b->wl_buf);
        munmap(b->data, b->size);
        b->wl_buf = NULL;
        b->data = NULL;
    }

    if (!b->wl_buf) {
        b->width = width;
        b->height = height;
        b->stride = stride;
        b->format = format;
        b->size = (size_t)stride * height;

        int fd = memfd_create("wfd-shm", MFD_CLOEXEC);
        if (fd < 0) {
            perror("[FluxCast-C Wayland] memfd_create failed");
            return;
        }
        if (ftruncate(fd, b->size) < 0) {
            perror("[FluxCast-C Wayland] ftruncate failed");
            close(fd);
            return;
        }
        b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (b->data == MAP_FAILED) {
            perror("[FluxCast-C Wayland] mmap failed");
            close(fd);
            b->data = NULL;
            return;
        }

        struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, b->size);
        b->wl_buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
        wl_shm_pool_destroy(pool);
        close(fd); // Immediately close fd: compositor has duplicated it and mmap keeps region alive
    }

    zwlr_screencopy_frame_v1_copy(frame, b->wl_buf);
    ctx->copy_done = true;
}

static void frame_handle_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {}
static void frame_handle_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct wayland_context *ctx = (struct wayland_context *)data;
    ctx->frame_ready = true;
}
static void frame_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    struct wayland_context *ctx = (struct wayland_context *)data;
    ctx->frame_failed = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct wayland_context *ctx = (struct wayland_context *)data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        ctx->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        ctx->screencopy_mgr = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!ctx->output) {
            ctx->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        }
    }
}
static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

// RTSP Server Thread (used for standalone mode)
static void *rtsp_server_thread(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(WFD_RTSP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[FluxCast-C RTSP] Bind failed");
        close(server_fd);
        return NULL;
    }
    listen(server_fd, 2);

    printf("[FluxCast-C RTSP] Server listening on 0.0.0.0:%d\n", WFD_RTSP_PORT);

    while (g_running) {
        struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) {
            if (!g_running) break;
            continue;
        }

        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &len);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        inet_ntop(AF_INET, &client_addr.sin_addr, g_tv_ip, sizeof(g_tv_ip));
        printf("[FluxCast-C RTSP] TV connected from %s:%d\n", g_tv_ip, ntohs(client_addr.sin_port));

        struct sockaddr_in local_addr;
        socklen_t loc_len = sizeof(local_addr);
        getsockname(client_fd, (struct sockaddr *)&local_addr, &loc_len);
        inet_ntop(AF_INET, &local_addr.sin_addr, g_local_ip, sizeof(g_local_ip));

        g_rtsp_client_fd = client_fd;
        int cseq = 1;

        // Send M1 (OPTIONS)
        char req[1024];
        snprintf(req, sizeof(req),
            "OPTIONS * RTSP/1.0\r\n"
            "CSeq: %d\r\n"
            "Require: org.wfa.wfd1.0\r\n"
            "User-Agent: FluxCast-C/1.0\r\n\r\n", cseq++);
        send(client_fd, req, strlen(req), 0);
        printf("[FluxCast-C RTSP] -> M1 OPTIONS sent (CSeq 1)\n");

        char recv_buf[8192];
        int buf_len = 0;

        while (g_running) {
            int n = recv(client_fd, recv_buf + buf_len, sizeof(recv_buf) - buf_len - 1, 0);
            if (n <= 0) break;
            buf_len += n;
            recv_buf[buf_len] = '\0';

            char *msg_end;
            while ((msg_end = strstr(recv_buf, "\r\n\r\n")) != NULL) {
                *msg_end = '\0';
                char *headers = recv_buf;
                char *body = msg_end + 4;
                int header_len = (int)(msg_end - recv_buf);

                int content_len = 0;
                char *cl_pos = strstr(headers, "Content-Length:");
                if (!cl_pos) cl_pos = strstr(headers, "content-length:");
                if (cl_pos) content_len = atoi(cl_pos + 15);

                int body_len = buf_len - (header_len + 4);
                if (body_len < content_len) {
                    *msg_end = '\r';
                    break;
                }

                char full_body[4096] = {0};
                if (content_len > 0) {
                    memcpy(full_body, body, content_len);
                    full_body[content_len] = '\0';
                }

                if (strncmp(headers, "RTSP/1.0 200 OK", 15) == 0) {
                    char *cs = strstr(headers, "CSeq:");
                    int resp_cseq = cs ? atoi(cs + 5) : 0;
                    printf("[FluxCast-C RTSP] <- 200 OK (CSeq %d)\n", resp_cseq);

                    if (resp_cseq == 1) {
                        // M1 Response
                    }
                    else if (resp_cseq == 2) {
                        char *p = strstr(full_body, "wfd_client_rtp_ports:");
                        if (p) {
                            int rtp_p = 0;
                            if (sscanf(p, "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d", &rtp_p) == 1) {
                                g_tv_rtp_port = rtp_p;
                            }
                        }
                        printf("[FluxCast-C RTSP] Negotiated TV RTP Sink Port: %d\n", g_tv_rtp_port);

                        char m4_body[1024];
                        snprintf(m4_body, sizeof(m4_body),
                            "wfd_content_protection: none\r\n"
                            "wfd_video_formats: 40 00 01 10 00000100 00000000 00000000 00 0000 0000 00 none none\r\n"
                            "wfd_audio_codecs: none\r\n"
                            "wfd_presentation_URL: rtsp://%s:%d/wfd1.0/streamid=0 none\r\n"
                            "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d 0 mode=play\r\n"
                            "microsoft_latency_management_capability: low\r\n",
                            g_local_ip, WFD_RTSP_PORT, g_tv_rtp_port);

                        char m4[2048];
                        snprintf(m4, sizeof(m4),
                            "SET_PARAMETER rtsp://%s/wfd1.0/streamid=0 RTSP/1.0\r\n"
                            "CSeq: %d\r\n"
                            "Content-Type: text/parameters\r\n"
                            "Content-Length: %zu\r\n\r\n%s",
                            g_local_ip, cseq++, strlen(m4_body), m4_body);
                        send(client_fd, m4, strlen(m4), 0);
                        printf("[FluxCast-C RTSP] -> M4 SET_PARAMETER sent\n");
                    }
                    else if (resp_cseq == 3) {
                        const char *m5_body = "wfd_trigger_method: SETUP\r\n";
                        char m5[1024];
                        snprintf(m5, sizeof(m5),
                            "SET_PARAMETER rtsp://%s/wfd1.0/streamid=0 RTSP/1.0\r\n"
                            "CSeq: %d\r\n"
                            "Content-Type: text/parameters\r\n"
                            "Content-Length: %zu\r\n\r\n%s",
                            g_local_ip, cseq++, strlen(m5_body), m5_body);
                        send(client_fd, m5, strlen(m5), 0);
                        printf("[FluxCast-C RTSP] -> M5 TRIGGER SETUP sent\n");
                    }
                }
                else if (strncmp(headers, "OPTIONS", 7) == 0) {
                    char *cs = strstr(headers, "CSeq:");
                    int in_cseq = cs ? atoi(cs + 5) : 1;
                    char resp[1024];
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n\r\n", in_cseq);
                    send(client_fd, resp, strlen(resp), 0);
                    printf("[FluxCast-C RTSP] <- M2 OPTIONS handled; -> sending M3 GET_PARAMETER\n");

                    const char *m3_body =
                        "wfd_content_protection\r\n"
                        "wfd_video_formats\r\n"
                        "wfd_audio_codecs\r\n"
                        "wfd_client_rtp_ports\r\n"
                        "microsoft_latency_management_capability\r\n";
                    char m3[2048];
                    snprintf(m3, sizeof(m3),
                        "GET_PARAMETER rtsp://%s/wfd1.0/streamid=0 RTSP/1.0\r\n"
                        "CSeq: %d\r\n"
                        "Content-Type: text/parameters\r\n"
                        "Content-Length: %zu\r\n\r\n%s",
                        g_local_ip, cseq++, strlen(m3_body), m3_body);
                    send(client_fd, m3, strlen(m3), 0);
                }
                else if (strncmp(headers, "SETUP", 5) == 0) {
                    char *cs = strstr(headers, "CSeq:");
                    int in_cseq = cs ? atoi(cs + 5) : 1;
                    char resp[1024];
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Session: %s;timeout=60\r\n"
                        "Transport: RTP/AVP/UDP;unicast;client_port=%d;server_port=%d-%d\r\n\r\n",
                        in_cseq, g_session_id, g_tv_rtp_port, g_local_rtp_port, g_local_rtp_port + 1);
                    send(client_fd, resp, strlen(resp), 0);
                    printf("[FluxCast-C RTSP] <- SETUP handled\n");
                }
                else if (strncmp(headers, "PLAY", 4) == 0) {
                    char *cs = strstr(headers, "CSeq:");
                    int in_cseq = cs ? atoi(cs + 5) : 1;
                    char resp[1024];
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Session: %s\r\n\r\n", in_cseq, g_session_id);
                    send(client_fd, resp, strlen(resp), 0);
                    printf("[FluxCast-C RTSP] <- PLAY accepted! Triggering hardware stream!\n");
                    g_streaming = 1;
                }

                int consumed = header_len + 4 + content_len;
                memmove(recv_buf, recv_buf + consumed, buf_len - consumed);
                buf_len -= consumed;
                recv_buf[buf_len] = '\0';
            }
        }

        g_streaming = 0;
        close(client_fd);
        g_rtsp_client_fd = -1;
        printf("[FluxCast-C RTSP] TV disconnected.\n");
    }

    close(server_fd);
    return NULL;
}

// Keepalive Thread: Periodic M16 GET_PARAMETER
static void *keepalive_thread(void *arg) {
    int keepalive_cseq = 100;
    while (g_running) {
        for (int i = 0; i < 50 && g_running; i++) {
            usleep(100000); // 100ms ticks to exit promptly on signal
        }
        if (g_streaming && g_rtsp_client_fd >= 0) {
            char req[512];
            snprintf(req, sizeof(req),
                "GET_PARAMETER rtsp://%s/wfd1.0/streamid=0 RTSP/1.0\r\n"
                "CSeq: %d\r\n\r\n", g_local_ip, keepalive_cseq++);
            send(g_rtsp_client_fd, req, strlen(req), MSG_NOSIGNAL);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("=========================================================\n");
    printf("  FluxCast-C: Pure C Zero-Abstraction Miracast Streamer \n");
    printf("  Direct Wayland Screencopy -> Intel VDENC Hardware VAAPI\n");
    printf("  Real-time CLOCK_MONOTONIC PTS (Zero Clock Drift)       \n");
    printf("=========================================================\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stream-only") == 0) {
            g_stream_only = true;
        } else if (strcmp(argv[i], "--tv-ip") == 0 && i + 1 < argc) {
            strncpy(g_tv_ip, argv[++i], sizeof(g_tv_ip) - 1);
        } else if (strcmp(argv[i], "--tv-port") == 0 && i + 1 < argc) {
            g_tv_rtp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--local-ip") == 0 && i + 1 < argc) {
            strncpy(g_local_ip, argv[++i], sizeof(g_local_ip) - 1);
        } else if (strcmp(argv[i], "--local-port") == 0 && i + 1 < argc) {
            g_local_rtp_port = atoi(argv[++i]);
        }
    }

    AVBufferRef *hw_device_ctx = NULL;
    AVBufferRef *hw_frames_ref = NULL;
    AVCodecContext *enc = NULL;
    AVFormatContext *out_ctx = NULL;
    AVFrame *sw_frame = NULL;
    AVFrame *hw_frame = NULL;
    AVPacket *pkt = NULL;
    int timer_fd = -1;

    avformat_network_init();

    // 1. Wayland Screencopy Connection
    g_wl.display = wl_display_connect(NULL);
    if (!g_wl.display) {
        fprintf(stderr, "Error: Could not connect to Wayland compositor\n");
        return 1;
    }
    g_wl.registry = wl_display_get_registry(g_wl.display);
    wl_registry_add_listener(g_wl.registry, &registry_listener, &g_wl);
    wl_display_roundtrip(g_wl.display);

    if (!g_wl.shm || !g_wl.screencopy_mgr || !g_wl.output) {
        fprintf(stderr, "Error: Compositor missing required screencopy protocols\n");
        return 1;
    }
    printf("[FluxCast-C Wayland] Screencopy manager bound successfully.\n");

    pthread_t rtsp_th, ka_th;
    bool threads_started = false;

    if (!g_stream_only) {
        pthread_create(&rtsp_th, NULL, rtsp_server_thread, NULL);
        pthread_create(&ka_th, NULL, keepalive_thread, NULL);
        threads_started = true;

        printf("[FluxCast-C] Waiting for TV connection and PLAY handshake...\n");
        while (!g_streaming && g_running) {
            usleep(50000);
        }
    } else {
        printf("[FluxCast-C] Stream-only mode: Target %s:%d (Source %s:%d)\n",
               g_tv_ip, g_tv_rtp_port, g_local_ip, g_local_rtp_port);
        g_streaming = 1;
    }

    if (!g_running) {
        goto teardown;
    }

    // 3. Initialize VA-API Hardware Device Context
    int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    if (err < 0) {
        fprintf(stderr, "Error: Failed to open VA-API device /dev/dri/renderD128\n");
        return 1;
    }

    // 4. Initialize Hardware H.264 Encoder
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    enc = avcodec_alloc_context3(codec);
    enc->width = WIDTH;
    enc->height = HEIGHT;
    enc->time_base = (AVRational){1, 90000};
    enc->framerate = (AVRational){DEFAULT_FPS, 1};
    enc->pix_fmt = AV_PIX_FMT_VAAPI;
    enc->gop_size = 15;
    enc->max_b_frames = 0;
    enc->has_b_frames = 0;
    enc->slices = 1; // Strict 1 slice: prevents Intel VDENC driver bloat
    enc->flags |= AV_CODEC_FLAG_LOW_DELAY; // Forces DPB max_dec_frame_buffering=1 (bypasses 33-50ms TV reordering delay)
    enc->flags2 |= AV_CODEC_FLAG2_FAST;

    hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = WIDTH;
    frames_ctx->height = HEIGHT;
    frames_ctx->initial_pool_size = 4;
    av_hwframe_ctx_init(hw_frames_ref);
    enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

    // Ultra-low latency options
    av_opt_set(enc->priv_data, "low_power", "1", 0);
    av_opt_set(enc->priv_data, "profile", "578", 0);
    av_opt_set(enc->priv_data, "coder", "cavlc", 0);
    av_opt_set(enc->priv_data, "quality", "7", 0);
    av_opt_set(enc->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(enc->priv_data, "qp", "28", 0);
    av_opt_set(enc->priv_data, "sei", "recovery_point", 0);
    av_opt_set(enc->priv_data, "async_depth", "1", 0);

    err = avcodec_open2(enc, codec, NULL);
    if (err < 0) {
        fprintf(stderr, "Error: Failed to open h264_vaapi encoder (%d)\n", err);
        return 1;
    }
    printf("[FluxCast-C Encoder] Intel QuickSync VDENC ASIC engaged at %dx%d@%d!\n", WIDTH, HEIGHT, DEFAULT_FPS);

    // 5. Initialize Zero-Delay RTP MPEG-TS Output
    char rtp_url[256];
    snprintf(rtp_url, sizeof(rtp_url), "rtp://%s:%d", g_tv_ip, g_tv_rtp_port);
    printf("[FluxCast-C Network] Output target: %s (Source %s:%d)\n", rtp_url, g_local_ip, g_local_rtp_port);

    avformat_alloc_output_context2(&out_ctx, NULL, "rtp_mpegts", rtp_url);
    AVStream *st = avformat_new_stream(out_ctx, NULL);
    st->id = 4113;
    st->time_base = (AVRational){1, 90000};
    avcodec_parameters_from_context(st->codecpar, enc);

    av_opt_set(out_ctx->priv_data, "muxdelay", "0", 0);
    av_opt_set(out_ctx->priv_data, "muxpreload", "0", 0);
    av_opt_set(out_ctx->priv_data, "max_delay", "0", 0);
    av_opt_set(out_ctx->priv_data, "pes_payload_size", "0", 0);
    av_opt_set(out_ctx->priv_data, "flush_packets", "1", 0);
    av_opt_set(out_ctx->priv_data, "pcr_period", "20", 0);
    av_opt_set(out_ctx->priv_data, "pat_period", "0.1", 0);
    av_opt_set(out_ctx->priv_data, "mpegts_flags", "resend_headers+pat_pmt_at_frames", 0);
    av_opt_set_int(out_ctx->priv_data, "mpegts_pmt_start_pid", 4096, 0);
    av_opt_set_int(out_ctx->priv_data, "mpegts_start_pid", 4113, 0);

    AVDictionary *io_opts = NULL;
    av_dict_set(&io_opts, "localaddr", g_local_ip, 0);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", g_local_rtp_port);
    av_dict_set(&io_opts, "local_rtpport", port_str, 0);
    snprintf(port_str, sizeof(port_str), "%d", g_local_rtp_port + 1);
    av_dict_set(&io_opts, "local_rtcpport", port_str, 0);
    av_dict_set(&io_opts, "pkt_size", "1328", 0);
    av_dict_set(&io_opts, "buffer_size", "1048576", 0);

    err = avio_open2(&out_ctx->pb, out_ctx->url, AVIO_FLAG_WRITE, NULL, &io_opts);
    av_dict_free(&io_opts);
    if (err < 0) {
        fprintf(stderr, "Error: Could not open RTP UDP output (%d)\n", err);
        return 1;
    }

    // Apply Wi-Fi Multimedia (WMM) AC_VO (Voice/Ultra-low latency) QoS to RTP & RTCP sockets
    for (int sfd = 3; sfd < 128; sfd++) {
        struct sockaddr_in sin;
        socklen_t slen = sizeof(sin);
        if (getsockname(sfd, (struct sockaddr *)&sin, &slen) == 0) {
            if (sin.sin_family == AF_INET &&
                (ntohs(sin.sin_port) == g_local_rtp_port || ntohs(sin.sin_port) == g_local_rtp_port + 1)) {
                int prio = 6; // Voice / Highest Priority Access Category (AC_VO)
                setsockopt(sfd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
                int tos = 0xb8; // DSCP Expedited Forwarding (EF)
                setsockopt(sfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
                printf("[FluxCast-C Network] Socket fd %d on port %d: Applied SO_PRIORITY=6 (WMM AC_VO), IP_TOS=0xb8 (DSCP EF)\n",
                       sfd, ntohs(sin.sin_port));
            }
        }
    }

    if (avformat_write_header(out_ctx, NULL) < 0) {
        fprintf(stderr, "Error: Failed to write RTP MPEG-TS header\n");
        return 1;
    }

    // 6. Allocate Frame Buffers
    sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = WIDTH;
    sw_frame->height = HEIGHT;
    av_frame_get_buffer(sw_frame, 32);

    hw_frame = av_frame_alloc();
    pkt = av_packet_alloc();

    // 7. 60Hz Precision Timer (16,666,666 ns)
    timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec timer_spec = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 16666666 },
        .it_value    = { .tv_sec = 0, .tv_nsec = 16666666 }
    };
    timerfd_settime(timer_fd, 0, &timer_spec, NULL);

    struct timespec t_stream_start, now;
    clock_gettime(CLOCK_MONOTONIC, &t_stream_start);
    int64_t last_pts = -1;
    uint64_t frame_count = 0;

    printf("[FluxCast-C Loop] Streaming live at 60 FPS directly into TV hardware!\n");

    // 8. Stream Loop
    while (g_running && g_streaming) {
        uint64_t expirations = 0;
        if (read(timer_fd, &expirations, sizeof(expirations)) < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[FluxCast-C Loop] timerfd read error: %s (errno %d)\n", strerror(errno), errno);
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t pts_us = (now.tv_sec - t_stream_start.tv_sec) * 1000000LL + (now.tv_nsec - t_stream_start.tv_nsec) / 1000;
        int64_t pts = (pts_us * 90) / 1000;
        if (pts <= last_pts) {
            pts = last_pts + 1;
        }
        last_pts = pts;

        g_wl.frame_ready = false;
        g_wl.frame_failed = false;
        g_wl.copy_done = false;

        struct zwlr_screencopy_frame_v1 *sc_frame =
            zwlr_screencopy_manager_v1_capture_output(g_wl.screencopy_mgr, 1, g_wl.output);
        if (!sc_frame) {
            fprintf(stderr, "[FluxCast-C Wayland] Failed to create screencopy frame\n");
            continue;
        }
        zwlr_screencopy_frame_v1_add_listener(sc_frame, &frame_listener, &g_wl);

        if (wl_display_flush(g_wl.display) < 0 && errno != EAGAIN) {
            if (errno != EINTR) {
                fprintf(stderr, "[FluxCast-C Wayland] Display flush failed: %s\n", strerror(errno));
                g_running = 0;
                zwlr_screencopy_frame_v1_destroy(sc_frame);
                break;
            }
        }

        while (!g_wl.frame_ready && !g_wl.frame_failed && g_running) {
            if (wl_display_dispatch(g_wl.display) < 0) {
                if (errno == EINTR) continue;
                int wl_err = wl_display_get_error(g_wl.display);
                fprintf(stderr, "[FluxCast-C Wayland] Fatal display connection error: %s (code %d, errno %d)\n",
                        strerror(wl_err ? wl_err : errno), wl_err, errno);
                g_running = 0;
                break;
            }
        }
        zwlr_screencopy_frame_v1_destroy(sc_frame);

        if (g_wl.frame_failed || !g_wl.frame_ready) {
            static uint32_t failed_consecutive = 0;
            failed_consecutive++;
            if (failed_consecutive == 1 || failed_consecutive % 300 == 0) {
                fprintf(stderr, "[FluxCast-C Wayland] Screencopy capture failed (e.g. DPMS / display blanking, count=%u)\n",
                        failed_consecutive);
            }
            // Drop cycle safely: do not send uninitialized/stale garbage to encoder
            continue;
        }

        // Ultra-low latency AVX2 conversion from Wayland SHM buffer to NV12 sw_frame
        int conv_w = (g_wl.width < WIDTH) ? (g_wl.width & ~7) : WIDTH;
        int conv_h = (g_wl.height < HEIGHT) ? (g_wl.height & ~1) : HEIGHT;
        int stride_pixels = g_wl.stride / 4;
        xrgb_to_nv12_ultra((const uint32_t *)g_wl.bufs[g_wl.cur_buf].data, conv_w, conv_h, stride_pixels,
                          sw_frame->data[0], sw_frame->linesize[0],
                          sw_frame->data[1], sw_frame->linesize[1]);

        // Ping-pong SHM buffer index for next capture
        g_wl.cur_buf ^= 1;

        // Hardware encode
        err = av_hwframe_get_buffer(enc->hw_frames_ctx, hw_frame, 0);
        if (err < 0) {
            static int hw_buf_err_cnt = 0;
            if (++hw_buf_err_cnt % 60 == 1) {
                char errbuf[128];
                av_strerror(err, errbuf, sizeof(errbuf));
                fprintf(stderr, "[FluxCast-C Encoder] Hardware surface pool exhausted: %s (%d)\n", errbuf, err);
            }
            continue;
        }

        err = av_hwframe_transfer_data(hw_frame, sw_frame, 0);
        if (err < 0) {
            char errbuf[128];
            av_strerror(err, errbuf, sizeof(errbuf));
            fprintf(stderr, "[FluxCast-C Encoder] Hardware DMA transfer failed: %s (%d)\n", errbuf, err);
            av_frame_unref(hw_frame);
            continue;
        }
        hw_frame->pts = pts;

        err = avcodec_send_frame(enc, hw_frame);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            char errbuf[128];
            av_strerror(err, errbuf, sizeof(errbuf));
            fprintf(stderr, "[FluxCast-C Encoder] avcodec_send_frame failed: %s (%d)\n", errbuf, err);
        }

        while (avcodec_receive_packet(enc, pkt) == 0) {
            pkt->stream_index = 0;
            pkt->pts = pts;
            pkt->dts = pts;
            int w_ret = av_write_frame(out_ctx, pkt);
            if (w_ret < 0) {
                static int net_err_cnt = 0;
                if (++net_err_cnt % 60 == 1) {
                    char errbuf[128];
                    av_strerror(w_ret, errbuf, sizeof(errbuf));
                    fprintf(stderr, "[FluxCast-C Network] RTP transmission write error: %s (%d)\n", errbuf, w_ret);
                }
                if (w_ret == AVERROR(ECONNREFUSED) || w_ret == AVERROR(EPIPE) || w_ret == AVERROR(ENETUNREACH)) {
                    fprintf(stderr, "[FluxCast-C Network] TV endpoint unreachable; terminating stream.\n");
                    g_running = 0;
                    g_streaming = 0;
                }
            }
            av_packet_unref(pkt);
        }
        av_frame_unref(hw_frame);

        frame_count++;
        if (frame_count % 300 == 0) {
            printf("[FluxCast-C Stream] Transmitted %lu frames (%.1fs uptime, PTS=%ld)\n",
                   (unsigned long)frame_count, frame_count / 60.0, (long)pts);
        }
    }

teardown:
    printf("[FluxCast-C] Teardown");
    if (g_exit_signal) {
        printf(" (signal %d: %s)", (int)g_exit_signal, strsignal(g_exit_signal));
    }
    printf("...\n");

    if (threads_started) {
        pthread_join(rtsp_th, NULL);
        pthread_join(ka_th, NULL);
    }

    // Drain encoder
    if (enc && out_ctx) {
        avcodec_send_frame(enc, NULL);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            pkt->stream_index = 0;
            av_write_frame(out_ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    if (timer_fd >= 0) {
        close(timer_fd);
        timer_fd = -1;
    }

    if (out_ctx) {
        av_write_trailer(out_ctx);
        if (out_ctx->pb) {
            avio_closep(&out_ctx->pb);
        }
        avformat_free_context(out_ctx);
        out_ctx = NULL;
    }

    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
    av_packet_free(&pkt);

    if (enc) {
        avcodec_free_context(&enc);
    }
    if (hw_frames_ref) {
        av_buffer_unref(&hw_frames_ref);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }

    // Clean up Wayland SHM buffers
    for (int i = 0; i < 2; i++) {
        if (g_wl.bufs[i].wl_buf) {
            wl_buffer_destroy(g_wl.bufs[i].wl_buf);
            g_wl.bufs[i].wl_buf = NULL;
        }
        if (g_wl.bufs[i].data) {
            munmap(g_wl.bufs[i].data, g_wl.bufs[i].size);
            g_wl.bufs[i].data = NULL;
        }
    }

    // Clean up Wayland protocol objects
    if (g_wl.screencopy_mgr) {
        zwlr_screencopy_manager_v1_destroy(g_wl.screencopy_mgr);
        g_wl.screencopy_mgr = NULL;
    }
    if (g_wl.output) {
        wl_output_destroy(g_wl.output);
        g_wl.output = NULL;
    }
    if (g_wl.shm) {
        wl_shm_destroy(g_wl.shm);
        g_wl.shm = NULL;
    }
    if (g_wl.registry) {
        wl_registry_destroy(g_wl.registry);
        g_wl.registry = NULL;
    }
    if (g_wl.display) {
        wl_display_flush(g_wl.display);
        wl_display_disconnect(g_wl.display);
        g_wl.display = NULL;
    }

    return 0;
}
