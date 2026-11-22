#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

int main() {
    int w = 1920, h = 1080;
    uint8_t *src_data[4];
    int src_linesize[4];
    av_image_alloc(src_data, src_linesize, w, h, AV_PIX_FMT_BGR0, 32);

    uint8_t *dst_data[4];
    int dst_linesize[4];
    av_image_alloc(dst_data, dst_linesize, w, h, AV_PIX_FMT_NV12, 32);

    struct SwsContext *sws = sws_getContext(w, h, AV_PIX_FMT_BGR0,
                                            w, h, AV_PIX_FMT_NV12,
                                            SWS_FAST_BILINEAR, NULL, NULL, NULL);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < 60; i++) {
        sws_scale(sws, (const uint8_t * const*)src_data, src_linesize, 0, h, dst_data, dst_linesize);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double elapsed_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_nsec - t1.tv_nsec) / 1000000.0;
    printf("60 frames converted in %.2f ms (%.2f ms per frame)!\n", elapsed_ms, elapsed_ms / 60.0);

    sws_freeContext(sws);
    av_freep(&src_data[0]);
    av_freep(&dst_data[0]);
    return 0;
}
