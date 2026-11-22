#include "color_convert.h"

#include <stdbool.h>
#include <stddef.h>
#include <immintrin.h>

static inline __m256i channel_16(__m256i p0, __m256i p1, int shift) {
    const __m256i mask = _mm256_set1_epi32(0xff);
    __m256i c0 = _mm256_and_si256(_mm256_srli_epi32(p0, shift), mask);
    __m256i c1 = _mm256_and_si256(_mm256_srli_epi32(p1, shift), mask);
    return _mm256_packus_epi32(c0, c1);
}

static inline __m256i luma_16(__m256i r, __m256i g, __m256i b) {
    __m256i rg = _mm256_add_epi16(_mm256_mullo_epi16(r, _mm256_set1_epi16(47)),
                                  _mm256_mullo_epi16(g, _mm256_set1_epi16(157)));
    __m256i y = _mm256_add_epi16(rg, _mm256_mullo_epi16(b, _mm256_set1_epi16(16)));
    y = _mm256_add_epi16(y, _mm256_set1_epi16(128 + (16 << 8)));
    return _mm256_srli_epi16(y, 8);
}

static inline __m256i average_2x2_16(__m256i row0, __m256i row1) {
    __m256i vertical = _mm256_add_epi16(row0, row1);
    __m256i pairs = _mm256_add_epi16(vertical, _mm256_srli_epi64(vertical, 16));
    return _mm256_srli_epi16(_mm256_add_epi16(pairs, _mm256_set1_epi16(2)), 2);
}

static inline __m256i chroma_cb_16(__m256i r, __m256i g, __m256i b) {
    __m256i rg = _mm256_add_epi16(_mm256_mullo_epi16(r, _mm256_set1_epi16(-26)),
                                  _mm256_mullo_epi16(g, _mm256_set1_epi16(-87)));
    __m256i cb = _mm256_add_epi16(rg, _mm256_mullo_epi16(b, _mm256_set1_epi16(112)));
    cb = _mm256_add_epi16(cb, _mm256_set1_epi16(128 + (128 << 8)));
    return _mm256_srli_epi16(cb, 8);
}

static inline __m256i chroma_cr_16(__m256i r, __m256i g, __m256i b) {
    __m256i rg = _mm256_add_epi16(_mm256_mullo_epi16(r, _mm256_set1_epi16(112)),
                                  _mm256_mullo_epi16(g, _mm256_set1_epi16(-102)));
    __m256i cr = _mm256_add_epi16(rg, _mm256_mullo_epi16(b, _mm256_set1_epi16(-10)));
    cr = _mm256_add_epi16(cr, _mm256_set1_epi16(128 + (128 << 8)));
    return _mm256_srli_epi16(cr, 8);
}

static inline void store_32(uint8_t *dst, __m256i value, bool non_temporal) {
    if (non_temporal) {
        _mm256_stream_si256((__m256i *)dst, value);
    } else {
        _mm256_storeu_si256((__m256i *)dst, value);
    }
}

void color_convert_xrgb_to_nv12_bt709(const uint32_t *src, int w, int h, int stride_pixels,
                                      uint8_t *y_plane, int y_stride,
                                      uint8_t *uv_plane, int uv_stride) {
    const __m256i restore_order = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
    const __m256i compact_even_bytes = _mm256_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1,
        0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
    const int vector_width = w & ~31;
    const bool non_temporal = (((uintptr_t)y_plane | (uintptr_t)uv_plane |
                                (uintptr_t)y_stride | (uintptr_t)uv_stride) & 31) == 0;

    for (int y = 0; y < h; y += 2) {
        const uint32_t *row0 = src + (size_t)y * stride_pixels;
        const uint32_t *row1 = src + (size_t)(y + 1) * stride_pixels;
        uint8_t *y_out0 = y_plane + (size_t)y * y_stride;
        uint8_t *y_out1 = y_plane + (size_t)(y + 1) * y_stride;
        uint8_t *uv_out = uv_plane + (size_t)(y / 2) * uv_stride;
        int x = 0;

        for (; x < vector_width; x += 32) {
            __m256i p00 = _mm256_loadu_si256((const __m256i *)(row0 + x));
            __m256i p01 = _mm256_loadu_si256((const __m256i *)(row0 + x + 8));
            __m256i p02 = _mm256_loadu_si256((const __m256i *)(row0 + x + 16));
            __m256i p03 = _mm256_loadu_si256((const __m256i *)(row0 + x + 24));
            __m256i p10 = _mm256_loadu_si256((const __m256i *)(row1 + x));
            __m256i p11 = _mm256_loadu_si256((const __m256i *)(row1 + x + 8));
            __m256i p12 = _mm256_loadu_si256((const __m256i *)(row1 + x + 16));
            __m256i p13 = _mm256_loadu_si256((const __m256i *)(row1 + x + 24));

            __m256i r0a = channel_16(p00, p01, 16);
            __m256i r0b = channel_16(p02, p03, 16);
            __m256i g0a = channel_16(p00, p01, 8);
            __m256i g0b = channel_16(p02, p03, 8);
            __m256i b0a = channel_16(p00, p01, 0);
            __m256i b0b = channel_16(p02, p03, 0);
            __m256i r1a = channel_16(p10, p11, 16);
            __m256i r1b = channel_16(p12, p13, 16);
            __m256i g1a = channel_16(p10, p11, 8);
            __m256i g1b = channel_16(p12, p13, 8);
            __m256i b1a = channel_16(p10, p11, 0);
            __m256i b1b = channel_16(p12, p13, 0);

            __m256i y0 = _mm256_packus_epi16(luma_16(r0a, g0a, b0a),
                                              luma_16(r0b, g0b, b0b));
            __m256i y1 = _mm256_packus_epi16(luma_16(r1a, g1a, b1a),
                                              luma_16(r1b, g1b, b1b));
            y0 = _mm256_permutevar8x32_epi32(y0, restore_order);
            y1 = _mm256_permutevar8x32_epi32(y1, restore_order);

            __m256i ra = average_2x2_16(r0a, r1a);
            __m256i rb = average_2x2_16(r0b, r1b);
            __m256i ga = average_2x2_16(g0a, g1a);
            __m256i gb = average_2x2_16(g0b, g1b);
            __m256i ba = average_2x2_16(b0a, b1a);
            __m256i bb = average_2x2_16(b0b, b1b);

            __m256i cb = _mm256_packus_epi16(chroma_cb_16(ra, ga, ba),
                                              chroma_cb_16(rb, gb, bb));
            __m256i cr = _mm256_packus_epi16(chroma_cr_16(ra, ga, ba),
                                              chroma_cr_16(rb, gb, bb));
            cb = _mm256_shuffle_epi8(cb, compact_even_bytes);
            cr = _mm256_shuffle_epi8(cr, compact_even_bytes);
            __m256i uv = _mm256_permutevar8x32_epi32(_mm256_unpacklo_epi8(cb, cr),
                                                   restore_order);

            store_32(y_out0 + x, y0, non_temporal);
            store_32(y_out1 + x, y1, non_temporal);
            store_32(uv_out + x, uv, non_temporal);
        }

        for (; x < w; x += 2) {
            uint32_t p00 = row0[x];
            uint32_t p01 = row0[x + 1];
            uint32_t p10 = row1[x];
            uint32_t p11 = row1[x + 1];
            int r00 = (p00 >> 16) & 0xff, g00 = (p00 >> 8) & 0xff, b00 = p00 & 0xff;
            int r01 = (p01 >> 16) & 0xff, g01 = (p01 >> 8) & 0xff, b01 = p01 & 0xff;
            int r10 = (p10 >> 16) & 0xff, g10 = (p10 >> 8) & 0xff, b10 = p10 & 0xff;
            int r11 = (p11 >> 16) & 0xff, g11 = (p11 >> 8) & 0xff, b11 = p11 & 0xff;

            y_out0[x] = (uint8_t)(((47 * r00 + 157 * g00 + 16 * b00 + 128) >> 8) + 16);
            y_out0[x + 1] = (uint8_t)(((47 * r01 + 157 * g01 + 16 * b01 + 128) >> 8) + 16);
            y_out1[x] = (uint8_t)(((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8) + 16);
            y_out1[x + 1] = (uint8_t)(((47 * r11 + 157 * g11 + 16 * b11 + 128) >> 8) + 16);

            int r = (r00 + r01 + r10 + r11 + 2) >> 2;
            int g = (g00 + g01 + g10 + g11 + 2) >> 2;
            int b = (b00 + b01 + b10 + b11 + 2) >> 2;
            uv_out[x] = (uint8_t)(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            uv_out[x + 1] = (uint8_t)(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }

    if (non_temporal && vector_width > 0) {
        _mm_sfence();
    }
}
