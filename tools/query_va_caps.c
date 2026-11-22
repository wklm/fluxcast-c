#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

int main() {
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) return 1;
    VADisplay dpy = vaGetDisplayDRM(fd);
    int maj, min;
    vaInitialize(dpy, &maj, &min);
    printf("VA-API Initialized: %d.%d\n", maj, min);

    VAProfile profiles[] = {
        VAProfileH264ConstrainedBaseline,
        VAProfileH264Main,
        VAProfileH264High
    };
    const char *names[] = {"ConstrainedBaseline", "Main", "High"};

    for (int i = 0; i < 3; i++) {
        printf("\n=== Profile: %s (LP entrypoint) ===\n", names[i]);
        VAConfigAttrib attribs[6];
        attribs[0].type = VAConfigAttribRTFormat;
        attribs[1].type = VAConfigAttribRateControl;
        attribs[2].type = VAConfigAttribEncQualityRange;
        attribs[3].type = VAConfigAttribEncIntraRefresh;
        attribs[4].type = VAConfigAttribEncMaxSlices;
        attribs[5].type = VAConfigAttribEncSliceStructure;

        vaGetConfigAttributes(dpy, profiles[i], VAEntrypointEncSliceLP, attribs, 6);
        printf("  RTFormat: 0x%08x\n", attribs[0].value);
        printf("  RateControl: 0x%08x (CQP=0x%x, CBR=0x%x, VBR=0x%x, ICQ=0x%x)\n",
               attribs[1].value, VA_RC_CQP, VA_RC_CBR, VA_RC_VBR, VA_RC_ICQ);
        printf("  QualityRange: %d levels (1=best, %d=fastest)\n", attribs[2].value, attribs[2].value);
        printf("  IntraRefresh: 0x%08x\n", attribs[3].value);
        printf("  MaxSlices: %d\n", attribs[4].value);
        printf("  SliceStructure: 0x%08x\n", attribs[5].value);
    }

    vaTerminate(dpy);
    close(fd);
    return 0;
}
