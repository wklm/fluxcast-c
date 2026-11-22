#ifndef COLOR_CONVERT_H
#define COLOR_CONVERT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void color_convert_xrgb_to_nv12_bt709(const uint32_t *src, int w, int h, int stride_pixels,
                                      uint8_t *y_plane, int y_stride,
                                      uint8_t *uv_plane, int uv_stride);

#ifdef __cplusplus
}
#endif

#endif
