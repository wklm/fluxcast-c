#include "color_convert.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void reference_convert(const uint32_t *src, int w, int h, int stride_pixels,
                              uint8_t *y_plane, int y_stride,
                              uint8_t *uv_plane, int uv_stride) {
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            uint32_t p[4] = {
                src[(size_t)y * stride_pixels + x],
                src[(size_t)y * stride_pixels + x + 1],
                src[(size_t)(y + 1) * stride_pixels + x],
                src[(size_t)(y + 1) * stride_pixels + x + 1]
            };
            int r[4], g[4], b[4];
            for (int i = 0; i < 4; i++) {
                r[i] = (p[i] >> 16) & 0xff;
                g[i] = (p[i] >> 8) & 0xff;
                b[i] = p[i] & 0xff;
                int row = y + (i >= 2);
                int col = x + (i & 1);
                y_plane[(size_t)row * y_stride + col] =
                    (uint8_t)(((47 * r[i] + 157 * g[i] + 16 * b[i] + 128) >> 8) + 16);
            }
            int ra = (r[0] + r[1] + r[2] + r[3] + 2) >> 2;
            int ga = (g[0] + g[1] + g[2] + g[3] + 2) >> 2;
            int ba = (b[0] + b[1] + b[2] + b[3] + 2) >> 2;
            size_t uv_offset = (size_t)(y / 2) * uv_stride + x;
            uv_plane[uv_offset] = (uint8_t)(((-26 * ra - 87 * ga + 112 * ba + 128) >> 8) + 128);
            uv_plane[uv_offset + 1] = (uint8_t)(((112 * ra - 102 * ga - 10 * ba + 128) >> 8) + 128);
        }
    }
}

static uint32_t random_state = 0x7092026u;

static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void *allocate_buffer(size_t size, bool aligned) {
    if (aligned) {
        void *ptr = NULL;
        size_t allocation = (size + 63) & ~(size_t)63;
        if (posix_memalign(&ptr, 64, allocation) != 0) return NULL;
        return ptr;
    }
    return malloc(size + 1);
}

static int run_case(int w, int h, int src_stride, int y_stride, int uv_stride,
                    bool unaligned_output, bool patterned) {
    size_t src_size = (size_t)src_stride * h;
    size_t y_size = (size_t)y_stride * h;
    size_t uv_size = (size_t)uv_stride * (h / 2);
    uint32_t *src = allocate_buffer(src_size * sizeof(*src), true);
    uint8_t *actual_y_alloc = allocate_buffer(y_size, !unaligned_output);
    uint8_t *actual_uv_alloc = allocate_buffer(uv_size, !unaligned_output);
    uint8_t *expected_y = allocate_buffer(y_size, true);
    uint8_t *expected_uv = allocate_buffer(uv_size, true);
    if (!src || !actual_y_alloc || !actual_uv_alloc || !expected_y || !expected_uv) {
        fprintf(stderr, "Allocation failed for %dx%d test\n", w, h);
        free(src);
        free(actual_y_alloc);
        free(actual_uv_alloc);
        free(expected_y);
        free(expected_uv);
        return 1;
    }

    static const uint32_t colors[] = {
        0xff000000, 0xffffffff, 0xffff0000, 0xff00ff00,
        0xff0000ff, 0xff00ffff, 0xffff00ff, 0xffffff00
    };
    for (size_t i = 0; i < src_size; i++) {
        src[i] = patterned ? colors[i % (sizeof(colors) / sizeof(colors[0]))] : next_random();
    }

    uint8_t *actual_y = actual_y_alloc + (unaligned_output ? 1 : 0);
    uint8_t *actual_uv = actual_uv_alloc + (unaligned_output ? 1 : 0);
    memset(actual_y_alloc, 0xa5, y_size + (unaligned_output ? 1 : 0));
    memset(actual_uv_alloc, 0x5a, uv_size + (unaligned_output ? 1 : 0));
    memset(expected_y, 0, y_size);
    memset(expected_uv, 0, uv_size);

    reference_convert(src, w, h, src_stride, expected_y, y_stride, expected_uv, uv_stride);
    color_convert_xrgb_to_nv12_bt709(src, w, h, src_stride, actual_y, y_stride,
                                     actual_uv, uv_stride);

    int failed = 0;
    for (int y = 0; y < h && !failed; y++) {
        for (int x = 0; x < w; x++) {
            size_t offset = (size_t)y * y_stride + x;
            if (actual_y[offset] != expected_y[offset]) {
                fprintf(stderr, "Y mismatch at %dx%d (%d,%d): got %u, expected %u\n",
                        w, h, x, y, actual_y[offset], expected_y[offset]);
                failed = 1;
                break;
            }
        }
    }
    for (int y = 0; y < h / 2 && !failed; y++) {
        for (int x = 0; x < w; x++) {
            size_t offset = (size_t)y * uv_stride + x;
            if (actual_uv[offset] != expected_uv[offset]) {
                fprintf(stderr, "UV mismatch at %dx%d (%d,%d): got %u, expected %u\n",
                        w, h, x, y, actual_uv[offset], expected_uv[offset]);
                failed = 1;
                break;
            }
        }
    }

    free(src);
    free(actual_y_alloc);
    free(actual_uv_alloc);
    free(expected_y);
    free(expected_uv);
    return failed;
}

static double elapsed_ms(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) * 1000.0 +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static int benchmark(void) {
    const int w = 1920, h = 1080, iterations = 120;
    size_t src_size = (size_t)w * h;
    uint32_t *src = allocate_buffer(src_size * sizeof(*src), true);
    uint8_t *y_plane = allocate_buffer(src_size, true);
    uint8_t *uv_plane = allocate_buffer(src_size / 2, true);
    if (!src || !y_plane || !uv_plane) {
        fprintf(stderr, "Benchmark allocation failed\n");
        free(src);
        free(y_plane);
        free(uv_plane);
        return 1;
    }
    for (size_t i = 0; i < src_size; i++) src[i] = next_random();

    struct timespec start, end;
    color_convert_xrgb_to_nv12_bt709(src, w, h, w, y_plane, w, uv_plane, w);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; i++) {
        color_convert_xrgb_to_nv12_bt709(src, w, h, w, y_plane, w, uv_plane, w);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("BT.709 AVX2 conversion: %.3f ms/frame at 1920x1080 (%d frames)\n",
           elapsed_ms(&start, &end) / iterations, iterations);

    free(src);
    free(y_plane);
    free(uv_plane);
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= run_case(32, 2, 32, 32, 32, false, true);
    failed |= run_case(64, 4, 70, 96, 96, false, false);
    failed |= run_case(34, 4, 40, 64, 64, false, false);
    failed |= run_case(62, 6, 68, 96, 96, true, false);
    failed |= run_case(2, 2, 4, 32, 32, false, true);
    if (failed) {
        fprintf(stderr, "BT.709 conversion tests failed\n");
        return 1;
    }
    printf("BT.709 conversion tests passed (vectors, tails, strides, unaligned output)\n");
    return benchmark();
}
