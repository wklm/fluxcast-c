#include <stdio.h>
#include <unistd.h>
#include <libavformat/avformat.h>

int main() {
    avformat_network_init();
    AVFormatContext *oc = NULL;
    const char *url = "rtp://192.168.179.1:1010?localaddr=192.168.179.200&local_rtpport=19002&pkt_size=1328";
    avformat_alloc_output_context2(&oc, NULL, "rtp_mpegts", url);
    int err = avio_open(&oc->pb, oc->url, AVIO_FLAG_WRITE);
    printf("avio_open result: %d\n", err);
    sleep(2);
    return 0;
}
