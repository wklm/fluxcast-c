#include "video_enc.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DETAIL_X 64
#define DETAIL_Y 64
#define DETAIL_SIZE 256
#define BENCH_FRAMES 60

typedef struct EncodeResult {
    int packet_size;
    int roi_max_regions;
    double median_us;
    double p95_us;
} EncodeResult;

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t aa = *(const uint64_t *)a;
    uint64_t bb = *(const uint64_t *)b;
    return (aa > bb) - (aa < bb);
}

static void generate_text_pattern(AVFrame *frame, int phase) {
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < frame->width; x++) {
            uint8_t value = 32;
            if (x >= DETAIL_X && x < DETAIL_X + DETAIL_SIZE &&
                y >= DETAIL_Y && y < DETAIL_Y + DETAIL_SIZE) {
                value = ((x + y + phase) & 1) ? 235 : 16;
            }
            frame->data[0][(size_t)y * frame->linesize[0] + x] = value;
        }
    }
    for (int y = 0; y < frame->height / 2; y++) {
        for (int x = 0; x < frame->width; x++) {
            frame->data[1][(size_t)y * frame->linesize[1] + x] = 128;
        }
    }
}

static int encode_sequence(const VideoEncRoi *roi, EncodeResult *result) {
    VideoEncoder enc;
    VideoEncConfig cfg;
    video_enc_config_default(&cfg);
    int err = video_enc_init(&enc, &cfg);
    if (err < 0) return err;
    result->roi_max_regions = enc.roi_max_regions;

    AVFrame *frame = video_enc_alloc_sw_frame(&enc);
    AVPacket *pkt = av_packet_alloc();
    uint64_t latencies[BENCH_FRAMES - 1];
    if (!frame || !pkt) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        video_enc_close(&enc);
        return AVERROR(ENOMEM);
    }

    for (int i = 0; i < BENCH_FRAMES && err >= 0; i++) {
        generate_text_pattern(frame, i);
        int64_t start = now_us();
        if (roi) {
            err = video_enc_send_frame_with_rois(&enc, frame, i * 1500, roi, 1);
        } else {
            err = video_enc_send_frame(&enc, frame, i * 1500);
        }
        if (err == 0) err = video_enc_receive_packet(&enc, pkt);
        if (err == 0) {
            if (i == 0) result->packet_size = pkt->size;
            else latencies[i - 1] = (uint64_t)(now_us() - start);
            av_packet_unref(pkt);
        }
    }
    if (err == 0) {
        qsort(latencies, BENCH_FRAMES - 1, sizeof(latencies[0]), compare_u64);
        result->median_us = latencies[(BENCH_FRAMES - 1) / 2] / 1000.0;
        result->p95_us = latencies[(int)((BENCH_FRAMES - 1) * 0.95)] / 1000.0;
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    video_enc_close(&enc);
    return err;
}

int main(void) {
    EncodeResult baseline = {0}, detail = {0}, elsewhere = {0};
    VideoEncRoi detail_roi = { DETAIL_X, DETAIL_Y, DETAIL_SIZE, DETAIL_SIZE, 3 };
    VideoEncRoi other_roi = { 1024, 64, DETAIL_SIZE, DETAIL_SIZE, 3 };

    int err = encode_sequence(NULL, &baseline);
    if (err >= 0) err = encode_sequence(&detail_roi, &detail);
    if (err >= 0) err = encode_sequence(&other_roi, &elsewhere);
    if (err < 0) {
        fprintf(stderr, "ROI hardware encode failed: %d\n", err);
        return 1;
    }
    if (detail.roi_max_regions == 0) {
        puts("ROI hardware encode skipped: driver has no ROI support");
        return 0;
    }

    printf("ROI regions supported: %d\n", detail.roi_max_regions);
    printf("Baseline IDR: %d bytes; detail ROI: %d bytes; unrelated ROI: %d bytes\n",
           baseline.packet_size, detail.packet_size, elsewhere.packet_size);
    printf("Encode p50/p95: baseline %.2f/%.2f ms; detail ROI %.2f/%.2f ms; unrelated ROI %.2f/%.2f ms\n",
           baseline.median_us, baseline.p95_us, detail.median_us, detail.p95_us,
           elsewhere.median_us, elsewhere.p95_us);
    if (detail.packet_size <= baseline.packet_size || detail.packet_size <= elsewhere.packet_size) {
        fprintf(stderr, "ROI did not selectively increase detail in the target region\n");
        return 1;
    }
    puts("ROI hardware encoding test passed");
    return 0;
}
