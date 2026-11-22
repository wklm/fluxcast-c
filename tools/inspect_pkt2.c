#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <libavformat/avformat.h>

int main(int argc, char **argv) {
    const char *file = argc > 1 ? argv[1] : "/tmp/test_dump.ts";
    AVFormatContext *ic = NULL;
    if (avformat_open_input(&ic, file, NULL, NULL) < 0) {
        printf("Failed to open %s\n", file);
        return 1;
    }
    AVPacket *pkt = av_packet_alloc();
    int count = 0;
    while (av_read_frame(ic, pkt) >= 0 && count < 4) {
        printf("=== PKT %d: size=%d, flags=%d ===\n", count, pkt->size, pkt->flags);
        for (int i = 0; i < pkt->size - 4; i++) {
            if (pkt->data[i] == 0 && pkt->data[i+1] == 0 && pkt->data[i+2] == 0 && pkt->data[i+3] == 1) {
                printf("  Start code 4-byte at offset %d: nal_unit_type=%d (first bytes: %02x %02x %02x %02x)\n",
                       i, pkt->data[i+4] & 0x1f, pkt->data[i+4], pkt->data[i+5], pkt->data[i+6], pkt->data[i+7]);
            }
        }
        printf("First 32 bytes hex:\n  ");
        for (int i = 0; i < (pkt->size < 32 ? pkt->size : 32); i++) {
            printf("%02x ", pkt->data[i]);
        }
        printf("\n");
        count++;
        av_packet_unref(pkt);
    }
    return 0;
}
