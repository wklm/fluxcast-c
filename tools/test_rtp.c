#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

int main() {
    avformat_network_init();
    AVFormatContext *out_ctx = NULL;
    int err = avformat_alloc_output_context2(&out_ctx, NULL, "rtp_mpegts", "rtp://127.0.0.1:12345?pkt_size=1328");
    if (err < 0 || !out_ctx) {
        printf("Failed to alloc output context: %d\n", err);
        return 1;
    }
    printf("Allocated rtp_mpegts output context!\n");

    AVStream *st = avformat_new_stream(out_ctx, NULL);
    st->id = 0;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_H264;
    st->codecpar->width = 1920;
    st->codecpar->height = 1080;

    av_opt_set(out_ctx->priv_data, "muxdelay", "0", 0);
    av_opt_set(out_ctx->priv_data, "muxpreload", "0", 0);
    av_opt_set(out_ctx->priv_data, "max_delay", "0", 0);
    av_opt_set(out_ctx->priv_data, "pes_payload_size", "0", 0);
    av_opt_set(out_ctx->priv_data, "flush_packets", "1", 0);
    av_opt_set(out_ctx->priv_data, "pcr_period", "20", 0);
    av_opt_set(out_ctx->priv_data, "pat_period", "0.1", 0);
    av_opt_set(out_ctx->priv_data, "mpegts_flags", "resend_headers+pat_pmt_at_frames", 0);
    av_opt_set_int(out_ctx->priv_data, "mpegts_pmt_start_pid", 4096, 0);
    av_opt_set_int(out_ctx->priv_data, "mpegts_start_pid", 4113, 0);

    err = avio_open(&out_ctx->pb, out_ctx->url, AVIO_FLAG_WRITE);
    if (err < 0) {
        printf("avio_open failed: %d\n", err);
        return 1;
    }
    printf("avio_open succeeded!\n");

    err = avformat_write_header(out_ctx, NULL);
    if (err < 0) {
        printf("write_header failed: %d\n", err);
        return 1;
    }
    printf("rtp_mpegts header written successfully with 0-delay!\n");

    av_write_trailer(out_ctx);
    avio_closep(&out_ctx->pb);
    avformat_free_context(out_ctx);
    return 0;
}
