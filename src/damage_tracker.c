#include "damage_tracker.h"

#include <string.h>

static int64_t roi_area(VideoEncRoi roi) {
    return (int64_t)roi.width * roi.height;
}

static bool regions_touch(VideoEncRoi a, VideoEncRoi b) {
    int a_right = a.x + a.width;
    int a_bottom = a.y + a.height;
    int b_right = b.x + b.width;
    int b_bottom = b.y + b.height;
    return a.x <= b_right && b.x <= a_right && a.y <= b_bottom && b.y <= a_bottom;
}

static VideoEncRoi roi_union(VideoEncRoi a, VideoEncRoi b) {
    int left = a.x < b.x ? a.x : b.x;
    int top = a.y < b.y ? a.y : b.y;
    int a_right = a.x + a.width;
    int b_right = b.x + b.width;
    int a_bottom = a.y + a.height;
    int b_bottom = b.y + b.height;
    int right = a_right > b_right ? a_right : b_right;
    int bottom = a_bottom > b_bottom ? a_bottom : b_bottom;
    return (VideoEncRoi) {
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
        .qp_delta = DAMAGE_TRACKER_QP_DELTA,
    };
}

static void update_coverage(DamageTracker *tracker) {
    int64_t total = 0;
    for (size_t i = 0; i < tracker->count; i++) total += roi_area(tracker->rois[i]);
    int64_t frame_area = (int64_t)tracker->width * tracker->height;
    tracker->suppress = total * 100 > frame_area * DAMAGE_TRACKER_MAX_COVERAGE_PERCENT;
}

void damage_tracker_init(DamageTracker *tracker, int width, int height) {
    if (!tracker) return;
    memset(tracker, 0, sizeof(*tracker));
    tracker->width = width;
    tracker->height = height;
}

void damage_tracker_set_dimensions(DamageTracker *tracker, int width, int height) {
    if (!tracker) return;
    if (tracker->width != width || tracker->height != height) {
        tracker->width = width;
        tracker->height = height;
        damage_tracker_reset(tracker);
    }
}

void damage_tracker_reset(DamageTracker *tracker) {
    if (!tracker) return;
    tracker->count = 0;
    tracker->suppress = false;
}

void damage_tracker_add(DamageTracker *tracker, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height) {
    if (!tracker || tracker->width <= 0 || tracker->height <= 0 || width == 0 || height == 0) return;

    uint64_t left = x;
    uint64_t top = y;
    uint64_t right = left + width;
    uint64_t bottom = top + height;
    if (left >= (uint32_t)tracker->width || top >= (uint32_t)tracker->height) return;
    if (right > (uint32_t)tracker->width) right = (uint32_t)tracker->width;
    if (bottom > (uint32_t)tracker->height) bottom = (uint32_t)tracker->height;

    left &= ~(uint64_t)15;
    top &= ~(uint64_t)15;
    right = (right + 15) & ~(uint64_t)15;
    bottom = (bottom + 15) & ~(uint64_t)15;
    if (right > (uint32_t)tracker->width) right = (uint32_t)tracker->width;
    if (bottom > (uint32_t)tracker->height) bottom = (uint32_t)tracker->height;
    if (left >= right || top >= bottom) return;

    VideoEncRoi current = {
        .x = (int)left,
        .y = (int)top,
        .width = (int)(right - left),
        .height = (int)(bottom - top),
        .qp_delta = DAMAGE_TRACKER_QP_DELTA,
    };

    for (size_t i = 0; i < tracker->count; ) {
        if (regions_touch(tracker->rois[i], current)) {
            current = roi_union(tracker->rois[i], current);
            tracker->rois[i] = tracker->rois[tracker->count - 1];
            tracker->count--;
            i = 0;
        } else {
            i++;
        }
    }

    if (tracker->count < DAMAGE_TRACKER_MAX_ROIS) {
        tracker->rois[tracker->count++] = current;
    } else {
        size_t best = 0;
        int64_t best_growth = INT64_MAX;
        for (size_t i = 0; i < tracker->count; i++) {
            VideoEncRoi merged = roi_union(tracker->rois[i], current);
            int64_t growth = roi_area(merged) - roi_area(tracker->rois[i]);
            if (growth < best_growth) {
                best_growth = growth;
                best = i;
            }
        }
        tracker->rois[best] = roi_union(tracker->rois[best], current);
    }
    update_coverage(tracker);
}

size_t damage_tracker_get_rois(const DamageTracker *tracker, VideoEncRoi *out, size_t max_rois) {
    if (!tracker || !out || max_rois == 0 || tracker->suppress) return 0;
    size_t count = tracker->count < max_rois ? tracker->count : max_rois;
    memcpy(out, tracker->rois, count * sizeof(*out));
    return count;
}
