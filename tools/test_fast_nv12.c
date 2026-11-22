#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <omp.h>

void xrgb_to_nv12_fast(const uint32_t *src, int w, int h, uint8_t *y_plane, uint8_t *uv_plane) {
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y += 2) {
        const uint32_t *row0 = src + y * w;
        const uint32_t *row1 = src + (y + 1) * w;
        uint8_t *y_out0 = y_plane + y * w;
        uint8_t *y_out1 = y_plane + (y + 1) * w;
        uint8_t *uv_out = uv_plane + (y / 2) * w;

        for (int x = 0; x < w; x += 2) {
            uint32_t p00 = row0[x];
            uint32_t p01 = row0[x + 1];
            uint32_t p10 = row1[x];
            uint32_t p11 = row1[x + 1];

            // Extract RGB (XRGB8888 in little-endian is B, G, R, X)
            int b00 = (p00) & 0xff, g00 = (p00 >> 8) & 0xff, r00 = (p00 >> 16) & 0xff;
            int b01 = (p01) & 0xff, g01 = (p01 >> 8) & 0xff, r01 = (p01 >> 16) & 0xff;
            int b10 = (p10) & 0xff, g10 = (p10 >> 8) & 0xff, r10 = (p10 >> 16) & 0xff;
            int b11 = (p11) & 0xff, g11 = (p11 >> 8) & 0xff, r11 = (p11 >> 16) & 0xff;

            // Y values
            y_out0[x]     = (uint8_t)(( (66 * r00 + 129 * g00 +  25 * b00 + 128) >> 8) + 16);
            y_out0[x + 1] = (uint8_t)(( (66 * r01 + 129 * g01 +  25 * b01 + 128) >> 8) + 16);
            y_out1[x]     = (uint8_t)(( (66 * r10 + 129 * g10 +  25 * b10 + 128) >> 8) + 16);
            y_out1[x + 1] = (uint8_t)(( (66 * r11 + 129 * g11 +  25 * b11 + 128) >> 8) + 16);

            // Average RGB for 2x2 UV block
            int r_avg = (r00 + r01 + r10 + r11) >> 2;
            int g_avg = (g00 + g01 + g10 + g11) >> 2;
            int b_avg = (b00 + b01 + b10 + b11) >> 2;

            uv_out[x]     = (uint8_t)(( (-38 * r_avg -  74 * g_avg + 112 * b_avg + 128) >> 8) + 128); // U
            uv_out[x + 1] = (uint8_t)(( (112 * r_avg -  94 * g_avg -  18 * b_avg + 128) >> 8) + 128); // V
        }
    }
}

int main() {
    int w = 1920, h = 1080;
    uint32_t *src = (uint32_t *)aligned_alloc(64, w * h * sizeof(uint32_t));
    uint8_t *y_plane = (uint8_t *)aligned_alloc(64, w * h);
    uint8_t *uv_plane = (uint8_t *)aligned_alloc(64, w * h / 2);

    for (int i = 0; i < w * h; i++) src[i] = 0xffa0b0c0;

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < 60; i++) {
        xrgb_to_nv12_fast(src, w, h, y_plane, uv_plane);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double elapsed_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_nsec - t1.tv_nsec) / 1000000.0;
    printf("Fast OpenMP conversion: 60 frames in %.2f ms (%.2f ms per frame)!\n", elapsed_ms, elapsed_ms / 60.0);

    free(src);
    free(y_plane);
    free(uv_plane);
    return 0;
}
