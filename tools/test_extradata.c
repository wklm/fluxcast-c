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
    avcodec_open2(enc, codec, NULL);

    printf("Extradata size immediately after open2: %d\n", enc->extradata_size);

    AVFrame *sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = 1920; sw_frame->height = 1080;
    av_frame_get_buffer(sw_frame, 32);

    AVFrame *hw_frame = av_frame_alloc();
    av_hwframe_get_buffer(enc->hw_frames_ctx, hw_frame, 0);
    av_hwframe_transfer_data(hw_frame, sw_frame, 0);

    avcodec_send_frame(enc, hw_frame);
    AVPacket *pkt = av_packet_alloc();
    avcodec_receive_packet(enc, pkt);

    printf("Extradata size after 1st frame: %d\n", enc->extradata_size);
    printf("1st packet size: %d, flags: 0x%x\n", pkt->size, pkt->flags);

    // Print first 30 bytes of first packet to see NAL units
    printf("1st packet hex: ");
    for (int i = 0; i < (pkt->size < 30 ? pkt->size : 30); i++) {
        printf("%02x ", pkt->data[i]);
    }
    printf("\n");

    return 0;
}
