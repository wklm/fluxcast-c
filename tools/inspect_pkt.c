#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <libavformat/avformat.h>

int main() {
    AVFormatContext *ic = NULL;
    if (avformat_open_input(&ic, "/tmp/test_dump.ts", NULL, NULL) < 0) return 1;
    AVPacket *pkt = av_packet_alloc();
    int count = 0;
    while (av_read_frame(ic, pkt) >= 0 && count < 5) {
        printf("PKT %d: size=%d, flags=%d\n", count, pkt->size, pkt->flags);
        for (int i = 0; i < pkt->size - 4 && i < 100; i++) {
            if (pkt->data[i] == 0 && pkt->data[i+1] == 0 && pkt->data[i+2] == 1) {
                printf("  Start code 3-byte at offset %d: nal_unit_type=%d\n", i, pkt->data[i+3] & 0x1f);
            } else if (pkt->data[i] == 0 && pkt->data[i+1] == 0 && pkt->data[i+2] == 0 && pkt->data[i+3] == 1) {
                printf("  Start code 4-byte at offset %d: nal_unit_type=%d\n", i, pkt->data[i+4] & 0x1f);
            }
        }
        count++;
        av_packet_unref(pkt);
    }
    return 0;
}
