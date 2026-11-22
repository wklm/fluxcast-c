#include <stdio.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

int main() {
    avformat_network_init();
    AVFormatContext *oc = NULL;
    char url[256] = "rtp://192.168.179.1:1010";
    avformat_alloc_output_context2(&oc, NULL, "rtp_mpegts", url);

    AVStream *st = avformat_new_stream(oc, NULL);
    st->id = 4113;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_H264;
    st->codecpar->width = 1920;
    st->codecpar->height = 1080;

    AVDictionary *io_opts = NULL;
    av_dict_set(&io_opts, "localaddr", "192.168.179.200", 0);
    av_dict_set(&io_opts, "local_rtpport", "19002", 0);
    av_dict_set(&io_opts, "local_rtcpport", "19003", 0);
    av_dict_set(&io_opts, "pkt_size", "1328", 0);

    int err = avio_open2(&oc->pb, oc->url, AVIO_FLAG_WRITE, NULL, &io_opts);
    av_dict_free(&io_opts);
    if (err < 0) {
        printf("avio_open2 failed: %d\n", err);
        return 1;
    }
    printf("avio_open2 succeeded!\n");
    avformat_write_header(oc, NULL);

    AVPacket *pkt = av_packet_alloc();
    pkt->data = av_malloc(100);
    memset(pkt->data, 0, 100);
    pkt->size = 100;
    pkt->stream_index = 0;
    pkt->pts = 0;
    pkt->dts = 0;

    av_write_frame(oc, pkt);
    printf("Packet written!\n");

    av_write_trailer(oc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    return 0;
}
