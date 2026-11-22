#ifndef DMABUF_CAPTURE_H
#define DMABUF_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-client.h>
#include <va/va.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DmaBufCaptureConfig {
    struct wl_display *display;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf_mgr;
    struct wl_output *output;
    VADisplay va_display;
    const char *drm_node;
    uint32_t width;
    uint32_t height;
} DmaBufCaptureConfig;

typedef struct DmaBufCapture DmaBufCapture;

/*
 * Initialize the zero-copy DMA-BUF screencopy subsystem and hardware VPP.
 * Pre-allocates double-buffered GBM buffers on the DRM render node,
 * creates Wayland DMA-BUF buffers (zwp_linux_dmabuf_v1),
 * imports into VA-API (vaCreateSurfaces / DRM_PRIME_2),
 * and configures the Intel VA-API VPP context for GPU hardware color conversion.
 *
 * Returns 0 on success, negative error code on failure.
 */
int dmabuf_capture_create(DmaBufCapture **out_cap, const DmaBufCaptureConfig *cfg);

/*
 * Capture one frame directly from Wayland compositor into GPU memory via DMA-BUF.
 * Blocks synchronously until the compositor signals frame completion.
 *
 * Returns 0 on success, negative error code on failure.
 */
int dmabuf_capture_frame(DmaBufCapture *cap);

/*
 * Execute hardware VPP color conversion (XRGB -> NV12) from the captured DMA-BUF
 * surface into the destination encoder surface in GPU hardware.
 * Runs entirely on Intel GPU fixed-function / EU hardware (~0.24ms, 0% CPU).
 *
 * Returns 0 on success, negative error code on failure.
 */
int dmabuf_capture_convert_vpp(DmaBufCapture *cap, VASurfaceID dst_surface);

/*
 * Returns the width of the captured screen.
 */
uint32_t dmabuf_capture_get_width(const DmaBufCapture *cap);

/*
 * Returns the height of the captured screen.
 */
uint32_t dmabuf_capture_get_height(const DmaBufCapture *cap);

/*
 * Release all GPU allocations, GBM devices, VA-API surfaces, and Wayland buffers.
 */
void dmabuf_capture_destroy(DmaBufCapture *cap);

#ifdef __cplusplus
}
#endif

#endif /* DMABUF_CAPTURE_H */
