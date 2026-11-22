#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <libavformat/avformat.h>

int main(int argc, char **argv) {
    const char *file = argc > 1 ? argv[1] : "/tmp/test_slices2.ts";
    AVFormatContext *ic = NULL;
    if (avformat_open_input(&ic, file, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open %s\n", file);
        return 1;
    }
    AVPacket *pkt = av_packet_alloc();
    int count = 0;
    while (av_read_frame(ic, pkt) >= 0) {
        if (pkt->stream_index == 0) {
            int nal_count = 0;
            int nal_types[256];
            for (int i = 0; i < pkt->size - 4; i++) {
                if (pkt->data[i] == 0 && pkt->data[i+1] == 0 && pkt->data[i+2] == 1) {
                    if (nal_count < 256) {
                        nal_types[nal_count++] = pkt->data[i+3] & 0x1f;
                    }
                } else if (pkt->data[i] == 0 && pkt->data[i+1] == 0 && pkt->data[i+2] == 0 && pkt->data[i+3] == 1) {
                    if (nal_count < 256) {
                        nal_types[nal_count++] = pkt->data[i+4] & 0x1f;
                    }
                }
            }
            printf("Frame %d (pts=%ld, size=%d, key=%d): %d NALs. First 10 types: ",
                   count, pkt->pts, pkt->size, !!(pkt->flags & AV_PKT_FLAG_KEY), nal_count);
            for (int k = 0; k < (nal_count < 10 ? nal_count : 10); k++) {
                printf("%d ", nal_types[k]);
            }
            printf("\n");
            count++;
            if (count >= 5) {
                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ic);
    return 0;
}
