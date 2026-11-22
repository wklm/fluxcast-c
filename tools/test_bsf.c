#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>

int main() {
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf) {
        printf("h264_mp4toannexb BSF not found\n");
        return 1;
    }
    printf("Found h264_mp4toannexb BSF!\n");

    AVBSFContext *bsf_ctx = NULL;
    av_bsf_alloc(bsf, &bsf_ctx);
    bsf_ctx->par_in->codec_id = AV_CODEC_ID_H264;
    bsf_ctx->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    bsf_ctx->par_in->width = 1920;
    bsf_ctx->par_in->height = 1080;
    av_bsf_init(bsf_ctx);
    printf("BSF initialized successfully!\n");

    av_bsf_free(&bsf_ctx);
    return 0;
}
