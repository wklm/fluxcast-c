#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

int main() {
    avformat_network_init();

    AVBufferRef *hw_device_ctx = NULL;
    int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    if (err < 0) return 1;

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->width = 1920;
    c->height = 1080;
    c->time_base = (AVRational){1, 90000};
    c->framerate = (AVRational){60, 1};
    c->pix_fmt = AV_PIX_FMT_VAAPI;
    c->gop_size = 15;
    c->max_b_frames = 0;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = 1920;
    frames_ctx->height = 1080;
    frames_ctx->initial_pool_size = 4;
    av_hwframe_ctx_init(hw_frames_ref);
    c->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

    av_opt_set(c->priv_data, "low_power", "1", 0);
    av_opt_set(c->priv_data, "profile", "578", 0);
    av_opt_set(c->priv_data, "coder", "cavlc", 0);
    av_opt_set(c->priv_data, "quality", "7", 0);
    av_opt_set(c->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(c->priv_data, "qp", "28", 0);
    av_opt_set(c->priv_data, "async_depth", "1", 0);
    av_opt_set(c->priv_data, "aud", "1", 0);

    avcodec_open2(c, codec, NULL);

    AVFormatContext *oc = NULL;
    avformat_alloc_output_context2(&oc, NULL, "rtp_mpegts", "rtp://127.0.0.1:19998?pkt_size=1328");
    AVStream *st = avformat_new_stream(oc, NULL);
    st->id = 0;
    st->time_base = (AVRational){1, 90000};
    avcodec_parameters_from_context(st->codecpar, c);

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
    sw_frame->width = 1920;
    sw_frame->height = 1080;
    av_frame_get_buffer(sw_frame, 32);

    AVFrame *hw_frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();

    struct timespec t_start, t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int i = 0; i < 60; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int64_t pts_us = (t1.tv_sec - t_start.tv_sec) * 1000000LL + (t1.tv_nsec - t_start.tv_nsec) / 1000;
        int64_t pts = (pts_us * 90) / 1000; // 90kHz

        av_hwframe_get_buffer(c->hw_frames_ctx, hw_frame, 0);
        av_hwframe_transfer_data(hw_frame, sw_frame, 0);
        hw_frame->pts = pts;

        avcodec_send_frame(c, hw_frame);
        while (avcodec_receive_packet(c, pkt) == 0) {
            pkt->stream_index = 0;
            pkt->pts = pts;
            pkt->dts = pts;
            av_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        av_frame_unref(hw_frame);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double elapsed_ms = (t2.tv_sec - t_start.tv_sec) * 1000.0 + (t2.tv_nsec - t_start.tv_nsec) / 1000000.0;
    printf("60 full frames transferred, VAAPI hardware-encoded, and packetized to RTP in %.2f ms!\n", elapsed_ms);
    printf("Average latency per frame inside C pipeline: %.2f ms!\n", elapsed_ms / 60.0);

    av_write_trailer(oc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    av_buffer_unref(&hw_frames_ref);
    av_buffer_unref(&hw_device_ctx);
    return 0;
}
