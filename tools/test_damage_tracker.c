#include "damage_tracker.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    DamageTracker tracker;
    VideoEncRoi rois[DAMAGE_TRACKER_MAX_ROIS];
    damage_tracker_init(&tracker, 1920, 1080);

    CHECK(damage_tracker_get_rois(&tracker, rois, DAMAGE_TRACKER_MAX_ROIS) == 0);
    damage_tracker_add(&tracker, 17, 19, 8, 9);
    CHECK(damage_tracker_get_rois(&tracker, rois, DAMAGE_TRACKER_MAX_ROIS) == 1);
    CHECK(rois[0].x == 16 && rois[0].y == 16);
    CHECK(rois[0].width == 16 && rois[0].height == 16);
    CHECK(rois[0].qp_delta == 3);

    damage_tracker_reset(&tracker);
    damage_tracker_add(&tracker, 16, 16, 16, 16);
    damage_tracker_add(&tracker, 32, 16, 16, 16);
    CHECK(damage_tracker_get_rois(&tracker, rois, DAMAGE_TRACKER_MAX_ROIS) == 1);
    CHECK(rois[0].x == 16 && rois[0].width == 32);

    damage_tracker_reset(&tracker);
    damage_tracker_add(&tracker, 0, 0, 16, 16);
    damage_tracker_add(&tracker, 64, 0, 16, 16);
    damage_tracker_add(&tracker, 128, 0, 16, 16);
    damage_tracker_add(&tracker, 192, 0, 16, 16);
    CHECK(tracker.count == DAMAGE_TRACKER_MAX_ROIS);
    CHECK(damage_tracker_get_rois(&tracker, rois, DAMAGE_TRACKER_MAX_ROIS) == 3);

    damage_tracker_reset(&tracker);
    damage_tracker_add(&tracker, 1904, 1066, 50, 50);
    CHECK(damage_tracker_get_rois(&tracker, rois, DAMAGE_TRACKER_MAX_ROIS) == 1);
    CHECK(rois[0].x == 1904 && rois[0].y == 1056);
    CHECK(rois[0].width == 16 && rois[0].height == 24);

    damage_tracker_reset(&tracker);
    damage_tracker_add(&tracker, 0, 0, 1920, 1080);
    CHECK(damage_tracker_get_rois(&tracker, rois, DAMAGE_TRACKER_MAX_ROIS) == 0);

    damage_tracker_add(&tracker, 0, 0, 16, 16);
    CHECK(tracker.count == 1);
    damage_tracker_set_dimensions(&tracker, 1280, 720);
    CHECK(tracker.count == 0);
    damage_tracker_add(&tracker, 1300, 10, 16, 16);
    CHECK(tracker.count == 0);

    puts("Damage tracker tests passed");
    return 0;
}
