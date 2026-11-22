#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

int main() {
    AVBufferRef *hw_device_ctx = NULL;
    av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width = 1920; enc->height = 1080;
    enc->time_base = (AVRational){1, 90000};
    enc->pix_fmt = AV_PIX_FMT_VAAPI;

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
    av_opt_set(enc->priv_data, "async_depth", "1", 0);
    av_opt_set(enc->priv_data, "aud", "1", 0);
    avcodec_open2(enc, codec, NULL);

    AVFrame *sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = 1920; sw_frame->height = 1080;
    av_frame_get_buffer(sw_frame, 32);

    AVFrame *hw_frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    for (int i = 0; i < 3; i++) {
        av_hwframe_get_buffer(enc->hw_frames_ctx, hw_frame, 0);
        av_hwframe_transfer_data(hw_frame, sw_frame, 0);
        hw_frame->pts = i * 1500;
        avcodec_send_frame(enc, hw_frame);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            printf("Got packet %d: size=%d, keyframe=%d\n", i, pkt->size, !!(pkt->flags & AV_PKT_FLAG_KEY));
            printf("Hex: ");
            for (int k = 0; k < (pkt->size < 25 ? pkt->size : 25); k++) printf("%02x ", pkt->data[k]);
            printf("\n");
            av_packet_unref(pkt);
        }
        av_frame_unref(hw_frame);
    }
    return 0;
}
