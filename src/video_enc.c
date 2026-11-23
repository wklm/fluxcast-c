#include "video_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext_vaapi.h>

static inline int64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000);
}

void video_enc_config_default(VideoEncConfig *config) {
    if (!config) return;
    config->width       = VIDEO_ENC_DEFAULT_WIDTH;
    config->height      = VIDEO_ENC_DEFAULT_HEIGHT;
    config->fps         = VIDEO_ENC_DEFAULT_FPS;
    config->qp          = VIDEO_ENC_DEFAULT_QP;
    config->gop_size    = VIDEO_ENC_DEFAULT_GOP_SIZE;
    config->slices      = VIDEO_ENC_DEFAULT_SLICES;
    config->quality     = VIDEO_ENC_DEFAULT_QUALITY;
    config->async_depth = VIDEO_ENC_DEFAULT_ASYNC_DEPTH;
    config->profile     = VIDEO_ENC_DEFAULT_PROFILE;
    config->coder       = VIDEO_ENC_DEFAULT_CODER;
    config->device_path = VIDEO_ENC_DEFAULT_DEVICE;
}

int video_enc_init(VideoEncoder *enc, const VideoEncConfig *config) {
    if (!enc) return AVERROR(EINVAL);
    memset(enc, 0, sizeof(*enc));

    if (config) {
        enc->config = *config;
    } else {
        video_enc_config_default(&enc->config);
    }

    if (enc->config.width <= 0)       enc->config.width       = VIDEO_ENC_DEFAULT_WIDTH;
    if (enc->config.height <= 0)      enc->config.height      = VIDEO_ENC_DEFAULT_HEIGHT;
    if (enc->config.fps <= 0)         enc->config.fps         = VIDEO_ENC_DEFAULT_FPS;
    if (enc->config.qp <= 0)          enc->config.qp          = VIDEO_ENC_DEFAULT_QP;
    if (enc->config.gop_size <= 0)    enc->config.gop_size    = VIDEO_ENC_DEFAULT_GOP_SIZE;
    if (enc->config.slices <= 0)      enc->config.slices      = VIDEO_ENC_DEFAULT_SLICES;
    if (enc->config.quality <= 0)     enc->config.quality     = VIDEO_ENC_DEFAULT_QUALITY;
    if (enc->config.async_depth <= 0) enc->config.async_depth = VIDEO_ENC_DEFAULT_ASYNC_DEPTH;
    if (enc->config.profile <= 0)     enc->config.profile     = VIDEO_ENC_DEFAULT_PROFILE;
    /* In Constrained Baseline (578), CABAC is forbidden by H.264 standard */
    if (enc->config.profile == 578)   enc->config.coder       = 0;
    if (!enc->config.device_path)     enc->config.device_path = VIDEO_ENC_DEFAULT_DEVICE;

    char errbuf[128];
    int err;

    /* 1. Initialize VA-API Hardware Device Context */
    err = av_hwdevice_ctx_create(&enc->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                 enc->config.device_path, NULL, 0);
    if (err < 0) {
        av_strerror(err, errbuf, sizeof(errbuf));
        fprintf(stderr, "[video_enc] Error: Failed to open VA-API device '%s': %s (%d)\n",
                enc->config.device_path, errbuf, err);
        video_enc_close(enc);
        return err;
    }

    /* 2. Locate VA-API H.264 Hardware Encoder */
    enc->codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!enc->codec) {
        fprintf(stderr, "[video_enc] Error: Encoder 'h264_vaapi' not found\n");
        video_enc_close(enc);
        return AVERROR_ENCODER_NOT_FOUND;
    }

    enc->codec_ctx = avcodec_alloc_context3(enc->codec);
    if (!enc->codec_ctx) {
        fprintf(stderr, "[video_enc] Error: Failed to allocate AVCodecContext\n");
        video_enc_close(enc);
        return AVERROR(ENOMEM);
    }

    /* 3. Configure VA-API Hardware Surface Frames Context */
    enc->hw_frames_ref = av_hwframe_ctx_alloc(enc->hw_device_ctx);
    if (!enc->hw_frames_ref) {
        fprintf(stderr, "[video_enc] Error: Failed to allocate hardware frames context\n");
        video_enc_close(enc);
        return AVERROR(ENOMEM);
    }

    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)enc->hw_frames_ref->data;
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = enc->config.width;
    frames_ctx->height    = enc->config.height;
    frames_ctx->initial_pool_size = 4;

    err = av_hwframe_ctx_init(enc->hw_frames_ref);
    if (err < 0) {
        av_strerror(err, errbuf, sizeof(errbuf));
        fprintf(stderr, "[video_enc] Error: Failed to initialize hwframe context: %s (%d)\n",
                errbuf, err);
        video_enc_close(enc);
        return err;
    }

    /* 4. Configure Encoder Parameters */
    AVCodecContext *c = enc->codec_ctx;
    c->width        = enc->config.width;
    c->height       = enc->config.height;
    c->time_base    = (AVRational){1, 90000};
    c->framerate    = (AVRational){enc->config.fps, 1};
    c->pix_fmt         = AV_PIX_FMT_VAAPI;
    c->gop_size        = enc->config.gop_size;
    c->max_b_frames    = 0;
    c->has_b_frames    = 0;
    c->colorspace      = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc       = AVCOL_TRC_BT709;
    c->color_range     = AVCOL_RANGE_MPEG;

    /* Enforce single slice: prevents +87.4% bitrate bloat from driver row rounding */
    c->slices = enc->config.slices;

    /* Enforce low delay flags: forces DPB max_dec_frame_buffering=1, num_reorder_frames=0
     * Bypasses the 33-50ms TV reordering queue */
    c->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    c->flags2 |= AV_CODEC_FLAG2_FAST;

    c->hw_frames_ctx = av_buffer_ref(enc->hw_frames_ref);
    if (!c->hw_frames_ctx) {
        video_enc_close(enc);
        return AVERROR(ENOMEM);
    }

    /* 5. Set Hardware ASIC Options (Intel VDENC Fixed-Function Pipeline) */
    char qp_str[16];
    char qual_str[16];
    char async_str[16];
    char prof_str[16];
    snprintf(qp_str, sizeof(qp_str), "%d", enc->config.qp);
    snprintf(qual_str, sizeof(qual_str), "%d", enc->config.quality);
    snprintf(async_str, sizeof(async_str), "%d", enc->config.async_depth);
    snprintf(prof_str, sizeof(prof_str), "%d", enc->config.profile);

    /* Enforce low_power=1: activates Intel VDENC ASIC directly, bypassing EU shaders */
    av_opt_set(c->priv_data, "low_power", "1", 0);

    /* Profile: 77 (Main) or 100 (High) or 578 (Constrained Baseline) */
    av_opt_set(c->priv_data, "profile", prof_str, 0);

    /* Entropy coding: cabac or cavlc */
    const char *coder_str = (enc->config.coder == 1) ? "cabac" : "cavlc";
    av_opt_set(c->priv_data, "coder", coder_str, 0);

    /* Fastest hardware quality pass: quality=7 */
    av_opt_set(c->priv_data, "quality", qual_str, 0);

    /* Use constant quantization without rate-control lookahead. */
    av_opt_set(c->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(c->priv_data, "qp", qp_str, 0);
    av_opt_set(c->priv_data, "rc_lookahead", "0", 0);

    /* Enforce recovery_point SEI: preserves sync recovery points needed by VIDAA OS
     * while stripping 500ms HRD compliance buffering delay */
    av_opt_set(c->priv_data, "sei", "recovery_point", 0);

    /* Enforce async_depth=1: strictly 1 frame in GPU flight for minimal latency */
    av_opt_set(c->priv_data, "async_depth", async_str, 0);

    /* Note: Do NOT set "aud": Intel driver's internal AUD generator corrupts slice byte-alignment!
     * The transport muxer automatically generates and inserts valid AUDs into each PES packet. */

    /* 6. Open Encoder */
    err = avcodec_open2(c, enc->codec, NULL);
    if (err < 0) {
        av_strerror(err, errbuf, sizeof(errbuf));
        fprintf(stderr, "[video_enc] Error: Failed to open h264_vaapi encoder: %s (%d)\n",
                errbuf, err);
        video_enc_close(enc);
        return err;
    }

    /* 7. Preallocate Hardware Frame Buffer Container */
    enc->hw_frame = av_frame_alloc();
    if (!enc->hw_frame) {
        fprintf(stderr, "[video_enc] Error: Failed to allocate hardware frame container\n");
        video_enc_close(enc);
        return AVERROR(ENOMEM);
    }

    VAProfile va_profile;
    switch (enc->config.profile) {
        case AV_PROFILE_H264_HIGH:                va_profile = VAProfileH264High; break;
        case AV_PROFILE_H264_MAIN:                va_profile = VAProfileH264Main; break;
        case AV_PROFILE_H264_CONSTRAINED_BASELINE: va_profile = VAProfileH264ConstrainedBaseline; break;
        default:                                  va_profile = VAProfileNone; break;
    }
    VADisplay va_dpy = video_enc_get_va_display(enc);
    if (va_dpy && va_profile != VAProfileNone) {
        VAConfigAttrib attr = { .type = VAConfigAttribEncROI };
        VAStatus status = vaGetConfigAttributes(va_dpy, va_profile, VAEntrypointEncSliceLP, &attr, 1);
        if (status == VA_STATUS_SUCCESS && attr.value != VA_ATTRIB_NOT_SUPPORTED) {
            VAConfigAttribValEncROI roi = { .value = attr.value };
            enc->roi_max_regions = roi.bits.num_roi_regions;
        }
    }

    return 0;
}

AVFrame *video_enc_alloc_sw_frame(const VideoEncoder *enc) {
    if (!enc || !enc->codec_ctx) return NULL;

    AVFrame *frame = av_frame_alloc();
    if (!frame) return NULL;

    frame->format          = AV_PIX_FMT_NV12;
    frame->width           = enc->config.width;
    frame->height          = enc->config.height;
    frame->colorspace      = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc       = AVCOL_TRC_BT709;
    frame->color_range     = AVCOL_RANGE_MPEG;

    int err = av_frame_get_buffer(frame, 32);
    if (err < 0) {
        av_frame_free(&frame);
        return NULL;
    }

    return frame;
}

static int attach_rois(VideoEncoder *enc, const VideoEncRoi *rois, size_t roi_count) {
    if (roi_count == 0 || enc->roi_max_regions == 0) return 0;
    if (!rois) return AVERROR(EINVAL);
    if ((int)roi_count > enc->roi_max_regions) roi_count = (size_t)enc->roi_max_regions;

    AVFrameSideData *sd = av_frame_new_side_data(enc->hw_frame, AV_FRAME_DATA_REGIONS_OF_INTEREST,
                                                 roi_count * sizeof(AVRegionOfInterest));
    if (!sd) return AVERROR(ENOMEM);

    for (size_t i = 0; i < roi_count; i++) {
        int64_t right = (int64_t)rois[i].x + rois[i].width;
        int64_t bottom = (int64_t)rois[i].y + rois[i].height;
        if (rois[i].x < 0 || rois[i].y < 0 || rois[i].width <= 0 || rois[i].height <= 0 ||
            right > enc->config.width || bottom > enc->config.height ||
            rois[i].qp_delta <= 0 || rois[i].qp_delta > 51) {
            return AVERROR(EINVAL);
        }
        AVRegionOfInterest *roi = (AVRegionOfInterest *)sd->data + i;
        *roi = (AVRegionOfInterest) {
            .self_size = sizeof(*roi),
            .top = rois[i].y,
            .bottom = (int)bottom,
            .left = rois[i].x,
            .right = (int)right,
            .qoffset = { -rois[i].qp_delta, 51 },
        };
    }
    return 0;
}

int video_enc_send_frame_with_rois(VideoEncoder *enc, AVFrame *sw_frame, int64_t pts,
                                   const VideoEncRoi *rois, size_t roi_count) {
    if (!enc || !enc->codec_ctx) return AVERROR(EINVAL);

    /* Flushing/draining encoder */
    if (!sw_frame) {
        int64_t t_flush0 = get_time_us();
        int err = avcodec_send_frame(enc->codec_ctx, NULL);
        int64_t t_flush1 = get_time_us();
        enc->last_dma_time_us = 0;
        enc->last_send_time_us = (t_flush1 - t_flush0);
        return err;
    }

    int64_t t0 = get_time_us();

    /* Retrieve hardware surface from preallocated pool */
    int err = av_hwframe_get_buffer(enc->hw_frames_ref, enc->hw_frame, 0);
    if (err < 0) {
        return err;
    }

    /* DMA transfer: NV12 Host RAM -> GPU VA-API surface */
    err = av_hwframe_transfer_data(enc->hw_frame, sw_frame, 0);
    if (err < 0) {
        av_frame_unref(enc->hw_frame);
        return err;
    }

    int64_t t1 = get_time_us();
    enc->last_dma_time_us = (t1 - t0);

    enc->hw_frame->pts = pts;
    err = attach_rois(enc, rois, roi_count);
    if (err < 0) {
        av_frame_unref(enc->hw_frame);
        return err;
    }

    /* Submit surface to Intel VDENC ASIC */
    int64_t t_send0 = get_time_us();
    err = avcodec_send_frame(enc->codec_ctx, enc->hw_frame);
    int64_t t_send1 = get_time_us();
    enc->last_send_time_us = (t_send1 - t_send0);

    av_frame_unref(enc->hw_frame);

    return err;
}

int video_enc_send_frame(VideoEncoder *enc, AVFrame *sw_frame, int64_t pts) {
    return video_enc_send_frame_with_rois(enc, sw_frame, pts, NULL, 0);
}

VADisplay video_enc_get_va_display(const VideoEncoder *enc) {
    if (!enc || !enc->hw_device_ctx) return NULL;
    AVHWDeviceContext *hwdev = (AVHWDeviceContext *)enc->hw_device_ctx->data;
    if (hwdev->type != AV_HWDEVICE_TYPE_VAAPI) return NULL;
    AVVAAPIDeviceContext *va_ctx = (AVVAAPIDeviceContext *)hwdev->hwctx;
    return va_ctx ? va_ctx->display : NULL;
}

int video_enc_get_hw_surface(VideoEncoder *enc, VASurfaceID *out_surface) {
    if (!enc || !enc->codec_ctx || !enc->hw_frames_ref || !enc->hw_frame) {
        return AVERROR(EINVAL);
    }
    av_frame_unref(enc->hw_frame);
    int err = av_hwframe_get_buffer(enc->hw_frames_ref, enc->hw_frame, 0);
    if (err < 0) return err;
    if (out_surface) {
        *out_surface = (VASurfaceID)(uintptr_t)enc->hw_frame->data[3];
    }
    return 0;
}

int video_enc_send_hw_surface_with_rois(VideoEncoder *enc, int64_t pts,
                                        const VideoEncRoi *rois, size_t roi_count) {
    if (!enc || !enc->codec_ctx || !enc->hw_frame) {
        return AVERROR(EINVAL);
    }
    enc->hw_frame->pts = pts;
    enc->last_dma_time_us = 0; // Zero-copy: 0 us host-to-device upload

    int err = attach_rois(enc, rois, roi_count);
    if (err < 0) {
        av_frame_unref(enc->hw_frame);
        return err;
    }

    int64_t t_send0 = get_time_us();
    err = avcodec_send_frame(enc->codec_ctx, enc->hw_frame);
    int64_t t_send1 = get_time_us();
    enc->last_send_time_us = (t_send1 - t_send0);

    av_frame_unref(enc->hw_frame);
    return err;
}

int video_enc_send_hw_surface(VideoEncoder *enc, int64_t pts) {
    return video_enc_send_hw_surface_with_rois(enc, pts, NULL, 0);
}

int video_enc_receive_packet(VideoEncoder *enc, AVPacket *pkt) {
    if (!enc || !enc->codec_ctx || !pkt) return AVERROR(EINVAL);
    return avcodec_receive_packet(enc->codec_ctx, pkt);
}

int video_enc_encode(VideoEncoder *enc, AVFrame *sw_frame, int64_t pts, AVPacket *pkt) {
    if (!enc || !enc->codec_ctx || !pkt) return AVERROR(EINVAL);

    int64_t t_start = get_time_us();

    int err = video_enc_send_frame(enc, sw_frame, pts);
    if (err < 0) return err;

    int64_t t_recv0 = get_time_us();
    err = video_enc_receive_packet(enc, pkt);
    int64_t t_end = get_time_us();

    enc->last_hw_time_us    = enc->last_send_time_us + (t_end - t_recv0);
    enc->last_total_time_us = (t_end - t_start);

    return err;
}

int video_enc_drain(VideoEncoder *enc, AVPacket *pkt) {
    if (!enc || !enc->codec_ctx || !pkt) return AVERROR(EINVAL);
    return avcodec_receive_packet(enc->codec_ctx, pkt);
}

void video_enc_close(VideoEncoder *enc) {
    if (!enc) return;

    if (enc->hw_frame) {
        av_frame_free(&enc->hw_frame);
    }
    if (enc->codec_ctx) {
        avcodec_free_context(&enc->codec_ctx);
    }
    if (enc->hw_frames_ref) {
        av_buffer_unref(&enc->hw_frames_ref);
    }
    if (enc->hw_device_ctx) {
        av_buffer_unref(&enc->hw_device_ctx);
    }

    memset(enc, 0, sizeof(*enc));
}
