#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

int main() {
    AVBufferRef *hw_device_ctx = NULL;
    int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    if (err < 0) {
        printf("Failed to create VAAPI device: %d\n", err);
        return 1;
    }
    printf("VAAPI device initialized successfully!\n");

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        printf("h264_vaapi encoder not found\n");
        return 1;
    }
    printf("Found h264_vaapi encoder!\n");

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->width = 1920;
    ctx->height = 1080;
    ctx->time_base = (AVRational){1, 60};
    ctx->framerate = (AVRational){60, 1};
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->gop_size = 15;
    ctx->max_b_frames = 0;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = 1920;
    frames_ctx->height = 1080;
    frames_ctx->initial_pool_size = 4;
    err = av_hwframe_ctx_init(hw_frames_ref);
    if (err < 0) {
        printf("Failed to init hwframe ctx: %d\n", err);
        return 1;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

    av_opt_set(ctx->priv_data, "low_power", "1", 0);
    av_opt_set(ctx->priv_data, "profile", "578", 0);
    av_opt_set(ctx->priv_data, "coder", "cavlc", 0);
    av_opt_set(ctx->priv_data, "quality", "7", 0);
    av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(ctx->priv_data, "qp", "28", 0);
    av_opt_set(ctx->priv_data, "async_depth", "1", 0);
    av_opt_set(ctx->priv_data, "aud", "1", 0);

    err = avcodec_open2(ctx, codec, NULL);
    if (err < 0) {
        printf("Failed to open h264_vaapi encoder: %d\n", err);
        return 1;
    }
    printf("h264_vaapi encoder opened successfully with low_power + cavlc + quality 7!\n");

    avcodec_free_context(&ctx);
    av_buffer_unref(&hw_frames_ref);
    av_buffer_unref(&hw_device_ctx);
    return 0;
}
