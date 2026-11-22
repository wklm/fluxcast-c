#include <stdio.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

int main() {
    avformat_network_init();
    AVFormatContext *oc = NULL;
    const char *url = "rtp://192.168.179.1:1010";
    avformat_alloc_output_context2(&oc, NULL, "rtp_mpegts", url);
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "localaddr", "192.168.179.200", 0);
    av_dict_set(&opts, "local_rtpport", "19002", 0);
    av_dict_set(&opts, "local_rtcpport", "19003", 0);
    av_dict_set(&opts, "pkt_size", "1328", 0);
    int err = avio_open2(&oc->pb, oc->url, AVIO_FLAG_WRITE, NULL, &opts);
    printf("avio_open2 result: %d\n", err);
    sleep(2);
    return 0;
}
