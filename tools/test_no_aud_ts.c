#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

int main() {
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
    // REMOVED "aud" option!
    avcodec_open2(enc, codec, NULL);

    AVFormatContext *oc = NULL;
    avformat_alloc_output_context2(&oc, NULL, "mpegts", "/tmp/test_clean.ts");
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
    // Fill sw_frame with some non-black data (e.g. gray)
    memset(sw_frame->data[0], 128, 1920 * 1080);
    memset(sw_frame->data[1], 128, 1920 * 540);

    AVFrame *hw_frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    for (int i = 0; i < 30; i++) {
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
    return 0;
}
