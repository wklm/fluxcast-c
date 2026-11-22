#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

#define BENCH_WIDTH   1920
#define BENCH_HEIGHT  1080
#define BENCH_FPS     60

static inline int64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000);
}

typedef struct {
    int profile;         /* 578, 77, 100 */
    const char *coder;   /* "cavlc", "cabac" */
    int quality;         /* 1 .. 7 */
    int qp;              /* 18 .. 26 */
    int gop_size;        /* 15, 30, 60, 120 */
    int low_power;       /* 1 */
    int num_frames;      /* e.g. 180 or 300 */
    const char *label;   /* Descriptive test name */
} TestConfig;

typedef struct {
    uint64_t min;
    uint64_t max;
    double mean;
    uint64_t median;
    uint64_t p90;
    uint64_t p95;
    uint64_t p99;
    double stddev;
} LatencyStats;

typedef struct {
    TestConfig cfg;
    LatencyStats lat_dma;
    LatencyStats lat_hw;
    LatencyStats lat_total;

    int idr_count;
    double idr_mean_bytes;
    int idr_max_bytes;

    int p_count;
    double p_mean_bytes;
    int p_max_bytes;

    double bitrate_mbps;
    double psnr_y_mean;
    double psnr_u_mean;
    double psnr_v_mean;
    double psnr_total_mean;
} TestResult;

static int compare_u64(const void *a, const void *b) {
    uint64_t v1 = *(const uint64_t *)a;
    uint64_t v2 = *(const uint64_t *)b;
    return (v1 > v2) - (v1 < v2);
}

static void compute_latency_stats(uint64_t *arr, int n, LatencyStats *s) {
    if (n <= 0) { memset(s, 0, sizeof(*s)); return; }
    uint64_t *sorted = (uint64_t *)malloc(n * sizeof(uint64_t));
    memcpy(sorted, arr, n * sizeof(uint64_t));
    qsort(sorted, n, sizeof(uint64_t), compare_u64);

    s->min = sorted[0];
    s->max = sorted[n - 1];
    s->median = sorted[n / 2];
    s->p90 = sorted[(int)(n * 0.90)];
    s->p95 = sorted[(int)(n * 0.95)];
    s->p99 = sorted[(int)(n * 0.99)];

    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += sorted[i];
    s->mean = (double)sum / n;

    double var_sum = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)sorted[i] - s->mean;
        var_sum += d * d;
    }
    s->stddev = sqrt(var_sum / n);
    free(sorted);
}

/*
 * High-realism screen content synthetic generator:
 * Desktop-like elements:
 * - High-contrast text lines and UI borders (simulating code editor / terminal / browser).
 * - Subtle gradients in the workspace background.
 * - Moving application window with rich internal text / sub-pixel edges.
 * This triggers realistic DCT high frequencies, motion vectors, and macroblock decisions!
 */
static void generate_screen_frame(AVFrame *frame, int frame_idx, int width, int height) {
    uint8_t *y_plane = frame->data[0];
    int y_stride = frame->linesize[0];
    uint8_t *uv_plane = frame->data[1];
    int uv_stride = frame->linesize[1];

    /* Moving window position (e.g. terminal window dragging across screen) */
    int win_w = 640;
    int win_h = 420;
    int max_x = width - win_w;
    int max_y = height - win_h;
    int speed_x = 10;
    int speed_y = 6;
    int pos_x = (frame_idx * speed_x) % (2 * max_x);
    if (pos_x > max_x) pos_x = 2 * max_x - pos_x;
    int pos_y = (frame_idx * speed_y) % (2 * max_y);
    if (pos_y > max_y) pos_y = 2 * max_y - pos_y;

    for (int y = 0; y < height; y++) {
        uint8_t *y_row = y_plane + y * y_stride;
        int in_win_y = (y >= pos_y && y < pos_y + win_h);
        int base_bg = 30 + ((y * 40) / height); // dark theme background

        for (int x = 0; x < width; x++) {
            if (in_win_y && (x >= pos_x && x < pos_x + win_w)) {
                int wx = x - pos_x;
                int wy = y - pos_y;
                if (wy < 30) {
                    // Window title bar: accent color / border
                    y_row[x] = (wx < 80) ? 200 : 70;
                } else if (wx < 4 || wx >= win_w - 4 || wy >= win_h - 4) {
                    // Window border
                    y_row[x] = 160;
                } else {
                    // Text / code lines inside window
                    int line_idx = (wy - 30) / 18;
                    int line_mod = (wy - 30) % 18;
                    if (line_mod >= 4 && line_mod <= 12) {
                        // Character simulation using hash pattern
                        int char_col = (wx - 20) / 10;
                        int char_mod = (wx - 20) % 10;
                        if (char_col >= 0 && char_col < 55 && char_mod >= 1 && char_mod <= 7) {
                            uint32_t h = (uint32_t)(line_idx * 131 + char_col * 17 + 7);
                            if ((h % 5) != 0 && (h & (1 << (char_mod + line_mod % 4)))) {
                                y_row[x] = 230; // White/bright code text
                            } else {
                                y_row[x] = 24;  // Editor background
                            }
                        } else {
                            y_row[x] = 24;
                        }
                    } else {
                        y_row[x] = 24;
                    }
                }
            } else {
                // Background screen elements: Taskbar at top (y < 40) or app icons / grid
                if (y < 40) {
                    y_row[x] = 45;
                    if (x % 120 < 4 && x < 800) y_row[x] = 120; // Taskbar icons
                } else if (y >= height - 30) {
                    y_row[x] = 50; // Status bar
                } else {
                    // Background wallpaper pattern
                    y_row[x] = (uint8_t)(base_bg + (((x & 64) ^ (y & 64)) ? 6 : 0));
                }
            }
        }
    }

    // Chroma NV12 plane
    for (int y = 0; y < height / 2; y++) {
        uint8_t *uv_row = uv_plane + y * uv_stride;
        int orig_y = y * 2;
        int in_win_y = (orig_y >= pos_y && orig_y < pos_y + win_h);

        for (int x = 0; x < width / 2; x++) {
            int orig_x = x * 2;
            if (in_win_y && (orig_x >= pos_x && orig_x < pos_x + win_w)) {
                int wy = orig_y - pos_y;
                if (wy < 30) {
                    uv_row[x * 2]     = 180; // Title bar U
                    uv_row[x * 2 + 1] = 80;  // Title bar V
                } else {
                    uv_row[x * 2]     = 128;
                    uv_row[x * 2 + 1] = 128;
                }
            } else {
                if (orig_y < 40) {
                    uv_row[x * 2]     = 140;
                    uv_row[x * 2 + 1] = 120;
                } else {
                    uv_row[x * 2]     = 128;
                    uv_row[x * 2 + 1] = 128;
                }
            }
        }
    }
}

/*
 * Compute PSNR between uncompressed original frame and decoded reconstructed frame.
 */
static void compute_psnr(const AVFrame *orig, const AVFrame *recon, int w, int h,
                         double *psnr_y, double *psnr_u, double *psnr_v, double *psnr_total) {
    uint64_t sse_y = 0;
    const uint8_t *orig_y = orig->data[0];
    const uint8_t *recon_y = recon->data[0];
    int stride_orig_y = orig->linesize[0];
    int stride_recon_y = recon->linesize[0];

    for (int y = 0; y < h; y++) {
        const uint8_t *row_o = orig_y + y * stride_orig_y;
        const uint8_t *row_r = recon_y + y * stride_recon_y;
        for (int x = 0; x < w; x++) {
            int diff = (int)row_o[x] - (int)row_r[x];
            sse_y += diff * diff;
        }
    }

    uint64_t sse_u = 0;
    uint64_t sse_v = 0;
    int cw = w / 2;
    int ch = h / 2;

    /* NV12 chroma */
    const uint8_t *orig_uv = orig->data[1];
    int stride_orig_uv = orig->linesize[1];

    /* Recon might be NV12 or YUV420P depending on decoder */
    if (recon->format == AV_PIX_FMT_NV12) {
        const uint8_t *recon_uv = recon->data[1];
        int stride_recon_uv = recon->linesize[1];
        for (int y = 0; y < ch; y++) {
            const uint8_t *row_o = orig_uv + y * stride_orig_uv;
            const uint8_t *row_r = recon_uv + y * stride_recon_uv;
            for (int x = 0; x < cw; x++) {
                int diff_u = (int)row_o[x * 2] - (int)row_r[x * 2];
                int diff_v = (int)row_o[x * 2 + 1] - (int)row_r[x * 2 + 1];
                sse_u += diff_u * diff_u;
                sse_v += diff_v * diff_v;
            }
        }
    } else if (recon->format == AV_PIX_FMT_YUV420P) {
        const uint8_t *recon_u = recon->data[1];
        const uint8_t *recon_v = recon->data[2];
        int stride_recon_u = recon->linesize[1];
        int stride_recon_v = recon->linesize[2];
        for (int y = 0; y < ch; y++) {
            const uint8_t *row_o = orig_uv + y * stride_orig_uv;
            const uint8_t *row_ru = recon_u + y * stride_recon_u;
            const uint8_t *row_rv = recon_v + y * stride_recon_v;
            for (int x = 0; x < cw; x++) {
                int diff_u = (int)row_o[x * 2] - (int)row_ru[x];
                int diff_v = (int)row_o[x * 2 + 1] - (int)row_rv[x];
                sse_u += diff_u * diff_u;
                sse_v += diff_v * diff_v;
            }
        }
    }

    double mse_y = (double)sse_y / (w * h);
    double mse_u = (double)sse_u / (cw * ch);
    double mse_v = (double)sse_v / (cw * ch);

    *psnr_y = (mse_y > 1e-10) ? (10.0 * log10((255.0 * 255.0) / mse_y)) : 99.99;
    *psnr_u = (mse_u > 1e-10) ? (10.0 * log10((255.0 * 255.0) / mse_u)) : 99.99;
    *psnr_v = (mse_v > 1e-10) ? (10.0 * log10((255.0 * 255.0) / mse_v)) : 99.99;

    double mse_total = (4.0 * mse_y + mse_u + mse_v) / 6.0;
    *psnr_total = (mse_total > 1e-10) ? (10.0 * log10((255.0 * 255.0) / mse_total)) : 99.99;
}

static int run_single_benchmark(const TestConfig *cfg, TestResult *res) {
    memset(res, 0, sizeof(*res));
    res->cfg = *cfg;

    AVBufferRef *hw_device_ctx = NULL;
    AVBufferRef *hw_frames_ref = NULL;
    AVCodecContext *enc_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *enc_codec = NULL;
    const AVCodec *dec_codec = NULL;
    AVFrame *sw_frame = NULL;
    AVFrame *hw_frame = NULL;
    AVFrame *dec_frame = NULL;
    AVPacket *pkt = NULL;
    int err = 0;

    int n_frames = cfg->num_frames;
    uint64_t *lat_dma = (uint64_t *)malloc(n_frames * sizeof(uint64_t));
    uint64_t *lat_hw  = (uint64_t *)malloc(n_frames * sizeof(uint64_t));
    uint64_t *lat_tot = (uint64_t *)malloc(n_frames * sizeof(uint64_t));
    uint64_t idr_bytes_sum = 0;
    uint64_t p_bytes_sum = 0;
    uint64_t total_bytes = 0;
    double psnr_y_sum = 0, psnr_u_sum = 0, psnr_v_sum = 0, psnr_tot_sum = 0;
    int psnr_frame_count = 0;

    /* 1. VA-API Device Init */
    err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    if (err < 0) { fprintf(stderr, "Failed to open VA-API device\n"); goto cleanup; }

    enc_codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!enc_codec) { fprintf(stderr, "Encoder h264_vaapi not found\n"); goto cleanup; }

    enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!enc_ctx) goto cleanup;

    hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = BENCH_WIDTH;
    frames_ctx->height    = BENCH_HEIGHT;
    frames_ctx->initial_pool_size = 4;
    err = av_hwframe_ctx_init(hw_frames_ref);
    if (err < 0) goto cleanup;

    enc_ctx->width        = BENCH_WIDTH;
    enc_ctx->height       = BENCH_HEIGHT;
    enc_ctx->time_base    = (AVRational){1, 90000};
    enc_ctx->framerate    = (AVRational){BENCH_FPS, 1};
    enc_ctx->pix_fmt      = AV_PIX_FMT_VAAPI;
    enc_ctx->gop_size     = cfg->gop_size;
    enc_ctx->max_b_frames = 0;
    enc_ctx->has_b_frames = 0;
    enc_ctx->slices       = 1;
    enc_ctx->flags       |= AV_CODEC_FLAG_LOW_DELAY;
    enc_ctx->flags2      |= AV_CODEC_FLAG2_FAST;
    enc_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

    char qp_s[16], q_s[16], prof_s[16], lp_s[16];
    snprintf(qp_s, sizeof(qp_s), "%d", cfg->qp);
    snprintf(q_s, sizeof(q_s), "%d", cfg->quality);
    snprintf(prof_s, sizeof(prof_s), "%d", cfg->profile);
    snprintf(lp_s, sizeof(lp_s), "%d", cfg->low_power);

    av_opt_set(enc_ctx->priv_data, "low_power", lp_s, 0);
    av_opt_set(enc_ctx->priv_data, "profile", prof_s, 0);
    av_opt_set(enc_ctx->priv_data, "coder", cfg->coder, 0);
    av_opt_set(enc_ctx->priv_data, "quality", q_s, 0);
    av_opt_set(enc_ctx->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(enc_ctx->priv_data, "qp", qp_s, 0);
    av_opt_set(enc_ctx->priv_data, "sei", "recovery_point", 0);
    av_opt_set(enc_ctx->priv_data, "async_depth", "1", 0);

    err = avcodec_open2(enc_ctx, enc_codec, NULL);
    if (err < 0) { fprintf(stderr, "Failed to open h264_vaapi encoder (%d)\n", err); goto cleanup; }

    /* Decoder Setup for reference PSNR */
    dec_codec = avcodec_find_decoder_by_name("h264");
    if (dec_codec) {
        dec_ctx = avcodec_alloc_context3(dec_codec);
        if (dec_ctx) {
            dec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            dec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
            avcodec_open2(dec_ctx, dec_codec, NULL);
        }
    }

    sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = BENCH_WIDTH;
    sw_frame->height = BENCH_HEIGHT;
    av_frame_get_buffer(sw_frame, 32);

    hw_frame = av_frame_alloc();
    dec_frame = av_frame_alloc();
    pkt = av_packet_alloc();

    /* Benchmark Loop */
    for (int i = 0; i < n_frames; i++) {
        generate_screen_frame(sw_frame, i, BENCH_WIDTH, BENCH_HEIGHT);

        int64_t t0 = get_time_us();
        av_hwframe_get_buffer(hw_frames_ref, hw_frame, 0);

        int64_t t_dma0 = get_time_us();
        av_hwframe_transfer_data(hw_frame, sw_frame, 0);
        int64_t t_dma1 = get_time_us();
        hw_frame->pts = (int64_t)i * (90000 / BENCH_FPS);

        int64_t t_hw0 = get_time_us();
        avcodec_send_frame(enc_ctx, hw_frame);
        av_frame_unref(hw_frame);

        err = avcodec_receive_packet(enc_ctx, pkt);
        int64_t t_end = get_time_us();

        if (err == 0) {
            lat_dma[i] = (t_dma1 - t_dma0);
            lat_hw[i]  = (t_end - t_hw0);
            lat_tot[i] = (t_end - t0);

            total_bytes += pkt->size;
            bool is_key = !!(pkt->flags & AV_PKT_FLAG_KEY);
            if (is_key) {
                res->idr_count++;
                idr_bytes_sum += pkt->size;
                if (pkt->size > res->idr_max_bytes) res->idr_max_bytes = pkt->size;
            } else {
                res->p_count++;
                p_bytes_sum += pkt->size;
                if (pkt->size > res->p_max_bytes) res->p_max_bytes = pkt->size;
            }

            /* Decode and measure PSNR */
            if (dec_ctx) {
                AVPacket dec_pkt;
                av_packet_ref(&dec_pkt, pkt);
                if (avcodec_send_packet(dec_ctx, &dec_pkt) == 0) {
                    while (avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
                        double py, pu, pv, ptot;
                        compute_psnr(sw_frame, dec_frame, BENCH_WIDTH, BENCH_HEIGHT, &py, &pu, &pv, &ptot);
                        psnr_y_sum += py;
                        psnr_u_sum += pu;
                        psnr_v_sum += pv;
                        psnr_tot_sum += ptot;
                        psnr_frame_count++;
                    }
                }
                av_packet_unref(&dec_pkt);
            }

            av_packet_unref(pkt);
        } else {
            fprintf(stderr, "Warning: frame %d receive failed: %d\n", i, err);
            lat_dma[i] = 0; lat_hw[i] = 0; lat_tot[i] = 0;
        }
    }

    compute_latency_stats(lat_dma, n_frames, &res->lat_dma);
    compute_latency_stats(lat_hw,  n_frames, &res->lat_hw);
    compute_latency_stats(lat_tot, n_frames, &res->lat_total);

    res->idr_mean_bytes = res->idr_count > 0 ? (double)idr_bytes_sum / res->idr_count : 0.0;
    res->p_mean_bytes   = res->p_count > 0 ? (double)p_bytes_sum / res->p_count : 0.0;
    res->bitrate_mbps   = ((double)total_bytes * 8.0 * BENCH_FPS) / ((double)n_frames * 1000000.0);

    if (psnr_frame_count > 0) {
        res->psnr_y_mean     = psnr_y_sum / psnr_frame_count;
        res->psnr_u_mean     = psnr_u_sum / psnr_frame_count;
        res->psnr_v_mean     = psnr_v_sum / psnr_frame_count;
        res->psnr_total_mean = psnr_tot_sum / psnr_frame_count;
    }

cleanup:
    free(lat_dma);
    free(lat_hw);
    free(lat_tot);
    if (pkt) av_packet_free(&pkt);
    if (sw_frame) av_frame_free(&sw_frame);
    if (hw_frame) av_frame_free(&hw_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (hw_frames_ref) av_buffer_unref(&hw_frames_ref);
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);

    return err;
}

static void print_header(void) {
    printf("%-26s | %-5s | %-5s | %2s | %2s | %3s | %7s | %8s | %7s | %8s | %8s | %8s | %6s\n",
           "Test Name", "Prof", "Coder", "Q", "QP", "GOP", "Bitrate", "IDR Mean", "P Mean", "DMA(p50)", "ASIC(p50)", "Tot(p50)", "PSNR-Y");
    printf("----------------------------------------------------------------------------------------------------------------------------------------------\n");
}

static void print_result(const TestResult *r) {
    printf("%-26s | %5d | %-5s | %2d | %2d | %3d | %5.2fMb | %6.0f B | %5.0f B | %5lu us | %6lu us | %6lu us | %5.2fdB\n",
           r->cfg.label, r->cfg.profile, r->cfg.coder, r->cfg.quality, r->cfg.qp, r->cfg.gop_size,
           r->bitrate_mbps, r->idr_mean_bytes, r->p_mean_bytes,
           r->lat_dma.median, r->lat_hw.median, r->lat_total.median, r->psnr_y_mean);
}

int main(int argc, char **argv) {
    av_log_set_level(AV_LOG_ERROR);
    int frames = 180; // 180 frames per configuration (3.0 seconds at 60fps)
    printf("==============================================================================================================================================\n");
    printf(" FluxCast-C: Hardware H.264 Encoder Empirical Frontier Benchmarking Suite\n");
    printf(" Platform: Intel Kaby Lake Gen9.5 GT2 (HD Graphics 620) | Linux 6.12 | /dev/dri/renderD128\n");
    printf(" Resolution: 1920x1080 @ 60 FPS | Frames per test: %d\n", frames);
    printf("==============================================================================================================================================\n\n");

    /* -------------------------------------------------------------
     * PART 1: QP SWEEP (QP 26, 24, 22, 20, 18)
     * Base config: Profile 578 (CBP), CAVLC, Quality 7, GOP 15
     * ------------------------------------------------------------- */
    printf(">>> PART 1: IMPACT OF QUANTIZATION PARAMETER (QP Sweep on Baseline/CAVLC/Q7/GOP15)\n");
    print_header();
    int qps[] = {26, 24, 22, 20, 18};
    for (int i = 0; i < 5; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "QP-%d (Baseline)", qps[i]);
        TestConfig cfg = {.profile = 578, .coder = "cavlc", .quality = 7, .qp = qps[i], .gop_size = 15, .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    printf("\n");

    /* -------------------------------------------------------------
     * PART 2: HARDWARE QUALITY SWEEP (Quality 7 vs 4 vs 1)
     * Testing at QP 26 and QP 22
     * ------------------------------------------------------------- */
    printf(">>> PART 2: IMPACT OF HARDWARE QUALITY SETTING (Quality 7 vs 4 vs 1)\n");
    print_header();
    int quals[] = {7, 4, 1};
    for (int i = 0; i < 3; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "Quality-%d (QP26)", quals[i]);
        TestConfig cfg = {.profile = 578, .coder = "cavlc", .quality = quals[i], .qp = 26, .gop_size = 15, .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    for (int i = 0; i < 3; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "Quality-%d (QP22)", quals[i]);
        TestConfig cfg = {.profile = 578, .coder = "cavlc", .quality = quals[i], .qp = 22, .gop_size = 15, .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    printf("\n");

    /* -------------------------------------------------------------
     * PART 3: ENTROPY CODING & PROFILES (CAVLC vs CABAC, Baseline vs Main vs High)
     * Testing at QP 26 and QP 22
     * ------------------------------------------------------------- */
    printf(">>> PART 3: ENTROPY CODING & H.264 PROFILES (Baseline 578 vs Main 77 vs High 100)\n");
    print_header();
    struct { int prof; const char *coder; const char *desc; } prof_tests[] = {
        {578, "cavlc", "Baseline 578 + CAVLC"},
        {77,  "cabac", "Main 77 + CABAC"},
        {100, "cavlc", "High 100 + CAVLC"},
        {100, "cabac", "High 100 + CABAC (8x8)"},
    };
    for (int i = 0; i < 4; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "%s (QP26)", prof_tests[i].desc);
        TestConfig cfg = {.profile = prof_tests[i].prof, .coder = prof_tests[i].coder, .quality = 7, .qp = 26, .gop_size = 15, .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    for (int i = 0; i < 4; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "%s (QP22)", prof_tests[i].desc);
        TestConfig cfg = {.profile = prof_tests[i].prof, .coder = prof_tests[i].coder, .quality = 7, .qp = 22, .gop_size = 15, .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    printf("\n");

    /* -------------------------------------------------------------
     * PART 4: GOP CADENCE SWEEP (GOP 15 vs 30 vs 60 vs 120)
     * Testing IDR overhead vs Bitrate
     * ------------------------------------------------------------- */
    printf(">>> PART 4: GOP STRUCTURE & IDR OVERHEAD (GOP 15 vs 30 vs 60 vs 120)\n");
    print_header();
    int gops[] = {15, 30, 60, 120};
    for (int i = 0; i < 4; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "GOP-%d (Baseline QP26)", gops[i]);
        TestConfig cfg = {.profile = 578, .coder = "cavlc", .quality = 7, .qp = 26, .gop_size = gops[i], .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    for (int i = 0; i < 4; i++) {
        char lbl[32]; snprintf(lbl, sizeof(lbl), "GOP-%d (High+CABAC QP22)", gops[i]);
        TestConfig cfg = {.profile = 100, .coder = "cabac", .quality = 7, .qp = 22, .gop_size = gops[i], .low_power = 1, .num_frames = frames, .label = lbl};
        TestResult r;
        run_single_benchmark(&cfg, &r);
        print_result(&r);
    }
    printf("\n");

    /* -------------------------------------------------------------
     * PART 5: PARETO FRONTIER CANDIDATES
     * Direct head-to-head comparison
     * ------------------------------------------------------------- */
    printf(">>> PART 5: PARETO FRONTIER HEAD-TO-HEAD CANDIDATES\n");
    print_header();
    TestConfig frontier_tests[] = {
        {.profile = 578, .coder = "cavlc", .quality = 7, .qp = 26, .gop_size = 15,  .low_power = 1, .num_frames = frames, .label = "Current Baseline (QP26)"},
        {.profile = 578, .coder = "cavlc", .quality = 7, .qp = 22, .gop_size = 30,  .low_power = 1, .num_frames = frames, .label = "Baseline QP22 GOP30"},
        {.profile = 100, .coder = "cabac", .quality = 7, .qp = 26, .gop_size = 30,  .low_power = 1, .num_frames = frames, .label = "High CABAC QP26 GOP30"},
        {.profile = 100, .coder = "cabac", .quality = 7, .qp = 22, .gop_size = 30,  .low_power = 1, .num_frames = frames, .label = "High CABAC QP22 GOP30"},
        {.profile = 100, .coder = "cabac", .quality = 7, .qp = 22, .gop_size = 60,  .low_power = 1, .num_frames = frames, .label = "High CABAC QP22 GOP60"},
        {.profile = 100, .coder = "cabac", .quality = 7, .qp = 20, .gop_size = 60,  .low_power = 1, .num_frames = frames, .label = "High CABAC QP20 GOP60"},
        {.profile = 100, .coder = "cabac", .quality = 4, .qp = 22, .gop_size = 30,  .low_power = 1, .num_frames = frames, .label = "High CABAC Q4 QP22 GOP30"},
        {.profile = 100, .coder = "cabac", .quality = 1, .qp = 22, .gop_size = 30,  .low_power = 1, .num_frames = frames, .label = "High CABAC Q1 QP22 GOP30"},
    };
    for (size_t i = 0; i < sizeof(frontier_tests)/sizeof(frontier_tests[0]); i++) {
        TestResult r;
        run_single_benchmark(&frontier_tests[i], &r);
        print_result(&r);
    }
    printf("\n");

    return 0;
}
