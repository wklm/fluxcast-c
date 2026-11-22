#ifndef VIDEO_ENC_H
#define VIDEO_ENC_H

#include <stdint.h>
#include <stdbool.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Default parameters optimized for Intel Gen9.5 GT2 (HD Graphics 620)
 * Fixed-Function VDENC ASIC low-latency streaming pipeline.
 */
#define VIDEO_ENC_DEFAULT_WIDTH       1920
#define VIDEO_ENC_DEFAULT_HEIGHT      1080
#define VIDEO_ENC_DEFAULT_FPS         60
#define VIDEO_ENC_DEFAULT_QP          26
#define VIDEO_ENC_DEFAULT_GOP_SIZE    15
#define VIDEO_ENC_DEFAULT_SLICES      1
#define VIDEO_ENC_DEFAULT_QUALITY     7
#define VIDEO_ENC_DEFAULT_ASYNC_DEPTH 1
#define VIDEO_ENC_DEFAULT_DEVICE      "/dev/dri/renderD128"

typedef struct VideoEncConfig {
    int width;              /* Frame width in pixels (default: 1920) */
    int height;             /* Frame height in pixels (default: 1080) */
    int fps;                /* Framerate in FPS (default: 60) */
    int qp;                 /* Constant QP for CQP mode (default: 26) */
    int gop_size;           /* GOP size / keyframe cadence (default: 15) */
    int slices;             /* Slices per frame (default: 1) */
    int quality;            /* Hardware quality level: 1 (best) to 7 (fastest) (default: 7) */
    int async_depth;        /* Frames in GPU flight (default: 1) */
    const char *device_path;/* VA-API render device node (default: "/dev/dri/renderD128") */
} VideoEncConfig;

typedef struct VideoEncoder {
    VideoEncConfig config;
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ref;
    AVCodecContext *codec_ctx;
    const AVCodec *codec;
    AVFrame *hw_frame;

    /* Microsecond timings of the most recent encode */
    int64_t last_dma_time_us;
    int64_t last_send_time_us;
    int64_t last_hw_time_us;
    int64_t last_total_time_us;
} VideoEncoder;

/*
 * Initialize configuration struct with low-latency Intel VDENC defaults.
 */
void video_enc_config_default(VideoEncConfig *config);

/*
 * Initialize VA-API hardware context, NV12 surface pool, and H.264 encoder.
 * Strictly enforces:
 *  - profile = 578 (Constrained Baseline / CHP)
 *  - coder = cavlc
 *  - rc_mode = CQP
 *  - qp = 26
 *  - low_power = 1 (Intel VDENC fixed-function ASIC)
 *  - async_depth = 1 (strictly 1 frame in GPU flight)
 *  - quality = 7 (fastest hardware pass)
 *  - slices = 1 (prevents +87.4% bitrate bloat from driver row rounding)
 *  - flags |= AV_CODEC_FLAG_LOW_DELAY, flags2 |= AV_CODEC_FLAG2_FAST
 *  - sei = recovery_point
 *  - gop_size = 15
 *
 * Returns 0 on success, negative AV error code on failure.
 */
int video_enc_init(VideoEncoder *enc, const VideoEncConfig *config);

/*
 * Allocate a software NV12 AVFrame configured for the encoder dimensions.
 * Returns pointer to allocated frame, or NULL on error.
 */
AVFrame *video_enc_alloc_sw_frame(const VideoEncoder *enc);

/*
 * Send a software NV12 frame to the hardware encoder.
 * Performs DMA upload into the VA-API surface pool and submits to VDENC ASIC.
 * Pass sw_frame = NULL to flush/drain the encoder pipeline.
 *
 * Returns 0 on success, negative AV error code on failure.
 */
int video_enc_send_frame(VideoEncoder *enc, AVFrame *sw_frame, int64_t pts);

/*
 * Receive an encoded H.264 packet from the hardware encoder.
 *
 * Returns 0 on success, AVERROR(EAGAIN) if more frames are required,
 * AVERROR_EOF if drained, or negative AV error code on failure.
 */
int video_enc_receive_packet(VideoEncoder *enc, AVPacket *pkt);

/*
 * Synchronous single-frame encode: uploads sw_frame, executes hardware encode,
 * and retrieves the resulting AVPacket.
 * Records last_dma_time_us, last_hw_time_us, and last_total_time_us.
 *
 * Returns 0 on success, negative AV error code on failure.
 */
int video_enc_encode(VideoEncoder *enc, AVFrame *sw_frame, int64_t pts, AVPacket *pkt);

/*
 * Drain remaining packets after flushing.
 */
int video_enc_drain(VideoEncoder *enc, AVPacket *pkt);

/*
 * Release all resources, hardware contexts, frame buffers, and encoder state.
 */
void video_enc_close(VideoEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_ENC_H */
