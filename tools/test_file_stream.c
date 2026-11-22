#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

static struct wl_shm *shm = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_mgr = NULL;
static struct wl_output *output = NULL;
static struct wl_buffer *wl_buf = NULL;
static uint32_t *shm_data = NULL;
static int shm_fd = -1;
static volatile int ready = 0;

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    if (strcmp(iface, wl_shm_interface.name) == 0) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) screencopy_mgr = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 1);
    else if (strcmp(iface, wl_output_interface.name) == 0) if (!output) output = wl_registry_bind(r, name, &wl_output_interface, 1);
}
static void reg_remove(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener r_list = { .global = reg_global, .global_remove = reg_remove };

static void fb_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride) {
    if (!wl_buf) {
        size_t size = stride * h;
        shm_fd = memfd_create("shm", MFD_CLOEXEC);
        ftruncate(shm_fd, size);
        shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        struct wl_shm_pool *p = wl_shm_create_pool(shm, shm_fd, size);
        wl_buf = wl_shm_pool_create_buffer(p, 0, w, h, stride, fmt);
        wl_shm_pool_destroy(p);
    }
    zwlr_screencopy_frame_v1_copy(f, wl_buf);
}
static void fb_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t fl) {}
static void fb_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c) { ready = 1; }
static void fb_failed(void *data, struct zwlr_screencopy_frame_v1 *f) { ready = -1; }
static const struct zwlr_screencopy_frame_v1_listener f_list = {
    .buffer = fb_buffer, .flags = fb_flags, .ready = fb_ready, .failed = fb_failed
};

static void xrgb_to_nv12(const uint32_t *src, int w, int h, uint8_t *y_out, uint8_t *uv_out) {
    for (int y = 0; y < h; y += 2) {
        const uint32_t *row0 = src + y * w;
        const uint32_t *row1 = src + (y + 1) * w;
        uint8_t *y0 = y_out + y * w;
        uint8_t *y1 = y_out + (y + 1) * w;
        uint8_t *uv = uv_out + (y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            uint32_t p00 = row0[x], p01 = row0[x+1], p10 = row1[x], p11 = row1[x+1];
            int b00 = p00&0xff, g00 = (p00>>8)&0xff, r00 = (p00>>16)&0xff;
            int b01 = p01&0xff, g01 = (p01>>8)&0xff, r01 = (p01>>16)&0xff;
            int b10 = p10&0xff, g10 = (p10>>8)&0xff, r10 = (p10>>16)&0xff;
            int b11 = p11&0xff, g11 = (p11>>8)&0xff, r11 = (p11>>16)&0xff;
            y0[x]   = (uint8_t)((( 66 * r00 + 129 * g00 +  25 * b00 + 128) >> 8) + 16);
            y0[x+1] = (uint8_t)((( 66 * r01 + 129 * g01 +  25 * b01 + 128) >> 8) + 16);
            y1[x]   = (uint8_t)((( 66 * r10 + 129 * g10 +  25 * b10 + 128) >> 8) + 16);
            y1[x+1] = (uint8_t)((( 66 * r11 + 129 * g11 +  25 * b11 + 128) >> 8) + 16);
            int r = (r00+r01+r10+r11)>>2, g = (g00+g01+g10+g11)>>2, b = (b00+b01+b10+b11)>>2;
            uv[x]   = (uint8_t)(((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128);
            uv[x+1] = (uint8_t)((( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128);
        }
    }
}

int main() {
    struct wl_display *d = wl_display_connect(NULL);
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &r_list, NULL);
    wl_display_roundtrip(d);

    AVBufferRef *hw_device_ctx = NULL;
    av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width = 1920; enc->height = 1080;
    enc->time_base = (AVRational){1, 90000};
    enc->framerate = (AVRational){60, 1};
    enc->pix_fmt = AV_PIX_FMT_VAAPI;
    enc->gop_size = 15;
    enc->max_b_frames = 0;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = 1920; frames_ctx->height = 1080;
    av_hwframe_ctx_init(hw_frames_ref);
    enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

    av_opt_set(enc->priv_data, "low_power", "1", 0);
    av_opt_set(enc->priv_data, "profile", "578", 0);
    av_opt_set(enc->priv_data, "coder", "cavlc", 0);
    av_opt_set(enc->priv_data, "quality", "7", 0);
    av_opt_set(enc->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(enc->priv_data, "qp", "28", 0);
    av_opt_set(enc->priv_data, "async_depth", "1", 0);
    avcodec_open2(enc, codec, NULL);

    AVFormatContext *oc = NULL;
    avformat_alloc_output_context2(&oc, NULL, "mpegts", "/tmp/test_dump.ts");
    AVStream *st = avformat_new_stream(oc, NULL);
    st->id = 0;
    st->time_base = (AVRational){1, 90000};
    avcodec_parameters_from_context(st->codecpar, enc);

    av_opt_set(oc->priv_data, "muxdelay", "0", 0);
    av_opt_set(oc->priv_data, "muxpreload", "0", 0);
    av_opt_set(oc->priv_data, "max_delay", "0", 0);
    av_opt_set(oc->priv_data, "pes_payload_size", "0", 0);
    av_opt_set(oc->priv_data, "flush_packets", "1", 0);
    av_opt_set(oc->priv_data, "pcr_period", "20", 0);
    av_opt_set(oc->priv_data, "pat_period", "0.1", 0);
    av_opt_set(oc->priv_data, "mpegts_flags", "resend_headers+pat_pmt_at_frames", 0);
    av_opt_set_int(oc->priv_data, "mpegts_pmt_start_pid", 4096, 0);
    av_opt_set_int(oc->priv_data, "mpegts_start_pid", 4113, 0);

    avio_open(&oc->pb, oc->url, AVIO_FLAG_WRITE);
    avformat_write_header(oc, NULL);

    AVFrame *sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = 1920; sw_frame->height = 1080;
    av_frame_get_buffer(sw_frame, 32);
    AVFrame *hw_frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    for (int i = 0; i < 30; i++) {
        ready = 0;
        struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output(screencopy_mgr, 1, output);
        zwlr_screencopy_frame_v1_add_listener(f, &f_list, NULL);
        wl_display_flush(d);
        while (!ready) wl_display_dispatch(d);
        zwlr_screencopy_frame_v1_destroy(f);

        xrgb_to_nv12(shm_data, 1920, 1080, sw_frame->data[0], sw_frame->data[1]);
        av_hwframe_get_buffer(enc->hw_frames_ctx, hw_frame, 0);
        av_hwframe_transfer_data(hw_frame, sw_frame, 0);
        hw_frame->pts = i * 1500;

        avcodec_send_frame(enc, hw_frame);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            pkt->stream_index = 0;
            pkt->pts = i * 1500;
            pkt->dts = i * 1500;
            av_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        av_frame_unref(hw_frame);
    }

    av_write_trailer(oc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    printf("Dumped 30 frames to /tmp/test_dump.ts\n");
    return 0;
}
