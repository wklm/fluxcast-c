#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dmabuf_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#define NUM_DMABUF_BUFFERS 3

struct dmabuf_buffer {
    struct gbm_bo *bo;
    int prime_fd;
    uint32_t stride;
    uint64_t modifier;
    struct wl_buffer *wl_buf;
    VASurfaceID va_surface;
    VABufferID pipe_buf;
};

struct DmaBufCapture {
    struct wl_display *display;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf_mgr;
    struct wl_output *output;
    VADisplay va_dpy;

    int drm_fd;
    struct gbm_device *gbm;
    bool va_dpy_owned;

    uint32_t width;
    uint32_t height;
    uint32_t format;

    struct dmabuf_buffer bufs[NUM_DMABUF_BUFFERS];
    int cur_buf;

    volatile int frame_ready;
    volatile int frame_failed;

    VAConfigID vpp_cfg;
    VAContextID vpp_ctx;
    VARectangle vpp_rect;
    DamageTracker damage;
};

static void sc_buffer(void *data, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {}
static void sc_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {}
static void sc_ready(void *data, struct zwlr_screencopy_frame_v1 *f,
                     uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    DmaBufCapture *cap = (DmaBufCapture *)data;
    cap->frame_ready = 1;
}
static void sc_failed(void *data, struct zwlr_screencopy_frame_v1 *f) {
    DmaBufCapture *cap = (DmaBufCapture *)data;
    cap->frame_failed = 1;
}
static void sc_damage(void *data, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    DmaBufCapture *cap = (DmaBufCapture *)data;
    damage_tracker_add(&cap->damage, x, y, width, height);
}
static void sc_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f,
                            uint32_t format, uint32_t width, uint32_t height) {
    DmaBufCapture *cap = (DmaBufCapture *)data;
    if ((cap->format && cap->format != format) ||
        (cap->width && cap->width != width) || (cap->height && cap->height != height)) {
        fprintf(stderr, "[dmabuf_capture] Output format or dimensions changed; restart capture\n");
        cap->frame_failed = 1;
        return;
    }
    cap->format = format;
    cap->width = width;
    cap->height = height;
}
static void sc_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f) {}

static const struct zwlr_screencopy_frame_v1_listener sc_listener = {
    .buffer = sc_buffer,
    .flags = sc_flags,
    .ready = sc_ready,
    .failed = sc_failed,
    .damage = sc_damage,
    .linux_dmabuf = sc_linux_dmabuf,
    .buffer_done = sc_buffer_done,
};

static void init_vpp_param(DmaBufCapture *cap, VASurfaceID surface,
                           VAProcPipelineParameterBuffer *param) {
    memset(param, 0, sizeof(*param));
    param->surface = surface;
    param->surface_region = &cap->vpp_rect;
    param->output_region = &cap->vpp_rect;
    param->surface_color_standard = VAProcColorStandardSRGB;
    param->input_color_properties.colour_primaries = 1;
    param->input_color_properties.transfer_characteristics = 13;
    param->input_color_properties.matrix_coefficients = 0;
    param->input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    param->output_color_standard = VAProcColorStandardBT709;
    param->output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
}

static int init_single_buffer(DmaBufCapture *cap, struct dmabuf_buffer *b) {
    b->bo = gbm_bo_create(cap->gbm, cap->width, cap->height, GBM_FORMAT_XRGB8888,
                          GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!b->bo) {
        b->bo = gbm_bo_create(cap->gbm, cap->width, cap->height, GBM_FORMAT_XRGB8888,
                              GBM_BO_USE_RENDERING);
    }
    if (!b->bo) {
        fprintf(stderr, "[dmabuf_capture] Error: gbm_bo_create failed\n");
        return -1;
    }

    b->prime_fd = gbm_bo_get_fd(b->bo);
    if (b->prime_fd < 0) {
        fprintf(stderr, "[dmabuf_capture] Error: gbm_bo_get_fd failed: %s\n", strerror(errno));
        return -1;
    }
    b->stride = gbm_bo_get_stride(b->bo);
    b->modifier = gbm_bo_get_modifier(b->bo);

    // Create Wayland buffer using zwp_linux_dmabuf_v1
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(cap->dmabuf_mgr);
    if (!params) {
        fprintf(stderr, "[dmabuf_capture] Error: Failed to create buffer parameters\n");
        return -ENOMEM;
    }
    zwp_linux_buffer_params_v1_add(params, b->prime_fd, 0, 0, b->stride,
                                  (uint32_t)(b->modifier >> 32), (uint32_t)(b->modifier & 0xffffffff));
    b->wl_buf = zwp_linux_buffer_params_v1_create_immed(params, cap->width, cap->height, cap->format, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (!b->wl_buf) {
        fprintf(stderr, "[dmabuf_capture] Error: zwp_linux_buffer_params_v1_create_immed failed\n");
        return -2;
    }

    // Import DMA-BUF Prime FD into VA-API as XRGB surface
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.fourcc = VA_FOURCC_BGRX;
    desc.width = cap->width;
    desc.height = cap->height;
    desc.num_objects = 1;
    desc.objects[0].fd = b->prime_fd;
    desc.objects[0].size = b->stride * cap->height;
    desc.objects[0].drm_format_modifier = b->modifier;
    desc.num_layers = 1;
    desc.layers[0].drm_format = DRM_FORMAT_XRGB8888;
    desc.layers[0].num_planes = 1;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = 0;
    desc.layers[0].pitch[0] = b->stride;

    VASurfaceAttrib attribs[2];
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &desc;

    b->va_surface = VA_INVALID_ID;
    VAStatus st = vaCreateSurfaces(cap->va_dpy, VA_RT_FORMAT_RGB32, cap->width, cap->height,
                                   &b->va_surface, 1, attribs, 2);

    /*
     * The PRIME fd has been exported to Wayland (sendmsg) and imported into VA-API (drmPrimeFDToHandle).
     * Both sides hold their respective kernel/driver references. Close the PRIME fd immediately
     * to eliminate lingering file descriptors.
     */
    close(b->prime_fd);
    b->prime_fd = -1;

    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[dmabuf_capture] Error: vaCreateSurfaces (DRM_PRIME_2) failed: %d (%s)\n",
                st, vaErrorStr(st));
        return -3;
    }

    return 0;
}

int dmabuf_capture_create(DmaBufCapture **out_cap, const DmaBufCaptureConfig *cfg) {
    if (out_cap) *out_cap = NULL;
    if (!out_cap || !cfg || !cfg->display || !cfg->screencopy_mgr || !cfg->dmabuf_mgr || !cfg->output) {
        return -EINVAL;
    }

    DmaBufCapture *cap = calloc(1, sizeof(*cap));
    if (!cap) return -ENOMEM;

    cap->display = cfg->display;
    cap->screencopy_mgr = cfg->screencopy_mgr;
    cap->dmabuf_mgr = cfg->dmabuf_mgr;
    cap->output = cfg->output;
    cap->va_dpy = cfg->va_display;
    cap->vpp_cfg = VA_INVALID_ID;
    cap->vpp_ctx = VA_INVALID_ID;
    cap->drm_fd = -1;

    for (int i = 0; i < NUM_DMABUF_BUFFERS; i++) {
        cap->bufs[i].prime_fd = -1;
        cap->bufs[i].va_surface = VA_INVALID_ID;
        cap->bufs[i].pipe_buf = VA_INVALID_ID;
    }

    // Step 1: Probe compositor screencopy support for linux-dmabuf
    struct zwlr_screencopy_frame_v1 *probe_frame =
        zwlr_screencopy_manager_v1_capture_output(cap->screencopy_mgr, 1, cap->output);
    if (!probe_frame) {
        dmabuf_capture_destroy(cap);
        return -ENOTSUP;
    }
    zwlr_screencopy_frame_v1_add_listener(probe_frame, &sc_listener, cap);
    wl_display_roundtrip(cap->display);
    zwlr_screencopy_frame_v1_destroy(probe_frame);

    if (cap->frame_failed || cap->format != DRM_FORMAT_XRGB8888 || cap->width == 0 || cap->height == 0) {
        fprintf(stderr, "[dmabuf_capture] Compositor did not advertise a compatible linux-dmabuf format\n");
        dmabuf_capture_destroy(cap);
        return -ENOTSUP;
    }

    if (cfg->width > 0 && cfg->height > 0) {
        cap->width = cfg->width;
        cap->height = cfg->height;
    }
    damage_tracker_init(&cap->damage, cap->width, cap->height);

    // Step 2: Open DRM device & GBM
    const char *drm_node = cfg->drm_node ? cfg->drm_node : "/dev/dri/renderD128";
    cap->drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
    if (cap->drm_fd < 0) {
        perror("[dmabuf_capture] open DRM node failed");
        dmabuf_capture_destroy(cap);
        return -errno;
    }

    cap->gbm = gbm_create_device(cap->drm_fd);
    if (!cap->gbm) {
        fprintf(stderr, "[dmabuf_capture] gbm_create_device failed\n");
        dmabuf_capture_destroy(cap);
        return -ENODEV;
    }

    // If VADisplay wasn't passed in, initialize one
    if (!cap->va_dpy) {
        cap->va_dpy = vaGetDisplayDRM(cap->drm_fd);
        if (!cap->va_dpy) {
            dmabuf_capture_destroy(cap);
            return -ENODEV;
        }
        int maj, min;
        if (vaInitialize(cap->va_dpy, &maj, &min) != VA_STATUS_SUCCESS) {
            fprintf(stderr, "[dmabuf_capture] vaInitialize failed\n");
            dmabuf_capture_destroy(cap);
            return -EIO;
        }
        cap->va_dpy_owned = true;
    }

    // Step 3: Initialize DMA-BUF buffers
    for (int i = 0; i < NUM_DMABUF_BUFFERS; i++) {
        int ret = init_single_buffer(cap, &cap->bufs[i]);
        if (ret < 0) {
            dmabuf_capture_destroy(cap);
            return ret;
        }
    }

    // Step 4: Configure Intel Hardware VPP (VideoProc) context
    VAStatus st = vaCreateConfig(cap->va_dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &cap->vpp_cfg);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[dmabuf_capture] vaCreateConfig VPP failed: %d (%s)\n", st, vaErrorStr(st));
        dmabuf_capture_destroy(cap);
        return -EIO;
    }

    st = vaCreateContext(cap->va_dpy, cap->vpp_cfg, cap->width, cap->height, 0, NULL, 0, &cap->vpp_ctx);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[dmabuf_capture] vaCreateContext VPP failed: %d (%s)\n", st, vaErrorStr(st));
        dmabuf_capture_destroy(cap);
        return -EIO;
    }

    cap->vpp_rect = (VARectangle){0, 0, (uint16_t)cap->width, (uint16_t)cap->height};
    for (int i = 0; i < NUM_DMABUF_BUFFERS; i++) {
        VAProcPipelineParameterBuffer param;
        init_vpp_param(cap, cap->bufs[i].va_surface, &param);
        st = vaCreateBuffer(cap->va_dpy, cap->vpp_ctx, VAProcPipelineParameterBufferType,
                            sizeof(param), 1, &param, &cap->bufs[i].pipe_buf);
        if (st != VA_STATUS_SUCCESS) {
            dmabuf_capture_destroy(cap);
            return -EIO;
        }
    }

    *out_cap = cap;
    return 0;
}

int dmabuf_capture_frame(DmaBufCapture *cap) {
    if (!cap) return -EINVAL;

    struct dmabuf_buffer *buf = &cap->bufs[cap->cur_buf];
    cap->frame_ready = 0;
    cap->frame_failed = 0;
    damage_tracker_reset(&cap->damage);

    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(cap->screencopy_mgr, 1, cap->output);
    if (!frame) return -EIO;

    zwlr_screencopy_frame_v1_add_listener(frame, &sc_listener, cap);
    zwlr_screencopy_frame_v1_copy_with_damage(frame, buf->wl_buf);

    if (wl_display_flush(cap->display) < 0 && errno != EAGAIN && errno != EINTR) {
        zwlr_screencopy_frame_v1_destroy(frame);
        return -errno;
    }

    while (!cap->frame_ready && !cap->frame_failed) {
        if (wl_display_dispatch(cap->display) < 0) {
            if (errno == EINTR) continue;
            zwlr_screencopy_frame_v1_destroy(frame);
            return -errno;
        }
    }
    zwlr_screencopy_frame_v1_destroy(frame);

    if (cap->frame_failed || !cap->frame_ready) {
        return -EAGAIN;
    }

    return 0;
}

int dmabuf_capture_convert_vpp(DmaBufCapture *cap, VASurfaceID dst_surface) {
    if (!cap || dst_surface == VA_INVALID_ID) return -EINVAL;

    struct dmabuf_buffer *buf = &cap->bufs[cap->cur_buf];

    VABufferID pipe_buf = buf->pipe_buf;
    if (pipe_buf == VA_INVALID_ID) return -EINVAL;

    VAStatus st = vaBeginPicture(cap->va_dpy, cap->vpp_ctx, dst_surface);
    if (st != VA_STATUS_SUCCESS) return -EIO;

    st = vaRenderPicture(cap->va_dpy, cap->vpp_ctx, &pipe_buf, 1);
    if (st != VA_STATUS_SUCCESS) {
        vaEndPicture(cap->va_dpy, cap->vpp_ctx);
        return -EIO;
    }

    st = vaEndPicture(cap->va_dpy, cap->vpp_ctx);

    // Advance buffer index for triple buffering
    cap->cur_buf = (cap->cur_buf + 1) % NUM_DMABUF_BUFFERS;

    return (st == VA_STATUS_SUCCESS) ? 0 : -EIO;
}

size_t dmabuf_capture_get_rois(const DmaBufCapture *cap, VideoEncRoi *rois, size_t max_rois) {
    if (!cap) return 0;
    return damage_tracker_get_rois(&cap->damage, rois, max_rois);
}

uint32_t dmabuf_capture_get_width(const DmaBufCapture *cap) {
    return cap ? cap->width : 0;
}

uint32_t dmabuf_capture_get_height(const DmaBufCapture *cap) {
    return cap ? cap->height : 0;
}

void dmabuf_capture_destroy(DmaBufCapture *cap) {
    if (!cap) return;

    if (cap->display) {
        wl_display_roundtrip(cap->display);
    }

    for (int i = 0; i < NUM_DMABUF_BUFFERS; i++) {
        if (cap->bufs[i].pipe_buf != VA_INVALID_ID) {
            vaDestroyBuffer(cap->va_dpy, cap->bufs[i].pipe_buf);
            cap->bufs[i].pipe_buf = VA_INVALID_ID;
        }
    }
    if (cap->vpp_ctx != VA_INVALID_ID) {
        vaDestroyContext(cap->va_dpy, cap->vpp_ctx);
        cap->vpp_ctx = VA_INVALID_ID;
    }
    if (cap->vpp_cfg != VA_INVALID_ID) {
        vaDestroyConfig(cap->va_dpy, cap->vpp_cfg);
        cap->vpp_cfg = VA_INVALID_ID;
    }

    for (int i = 0; i < NUM_DMABUF_BUFFERS; i++) {
        if (cap->bufs[i].va_surface != VA_INVALID_ID) {
            vaDestroySurfaces(cap->va_dpy, &cap->bufs[i].va_surface, 1);
            cap->bufs[i].va_surface = VA_INVALID_ID;
        }
        if (cap->bufs[i].wl_buf) {
            wl_buffer_destroy(cap->bufs[i].wl_buf);
            cap->bufs[i].wl_buf = NULL;
        }
        if (cap->bufs[i].bo) {
            gbm_bo_destroy(cap->bufs[i].bo);
            cap->bufs[i].bo = NULL;
        }
        if (cap->bufs[i].prime_fd >= 0) {
            close(cap->bufs[i].prime_fd);
            cap->bufs[i].prime_fd = -1;
        }
    }

    if (cap->gbm) {
        gbm_device_destroy(cap->gbm);
        cap->gbm = NULL;
    }
    if (cap->va_dpy_owned && cap->va_dpy) {
        vaTerminate(cap->va_dpy);
        cap->va_dpy = NULL;
    }
    if (cap->drm_fd >= 0) {
        close(cap->drm_fd);
        cap->drm_fd = -1;
    }

    if (cap->display) {
        wl_display_flush(cap->display);
    }

    free(cap);
}
