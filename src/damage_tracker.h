#ifndef DAMAGE_TRACKER_H
#define DAMAGE_TRACKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "video_enc.h"

#define DAMAGE_TRACKER_MAX_ROIS VIDEO_ENC_MAX_ROIS
#define DAMAGE_TRACKER_QP_DELTA 3
#define DAMAGE_TRACKER_MAX_COVERAGE_PERCENT 25

typedef struct DamageTracker {
    int width;
    int height;
    VideoEncRoi rois[DAMAGE_TRACKER_MAX_ROIS];
    size_t count;
    bool suppress;
} DamageTracker;

void damage_tracker_init(DamageTracker *tracker, int width, int height);
void damage_tracker_set_dimensions(DamageTracker *tracker, int width, int height);
void damage_tracker_reset(DamageTracker *tracker);
void damage_tracker_add(DamageTracker *tracker, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height);
size_t damage_tracker_get_rois(const DamageTracker *tracker, VideoEncRoi *out, size_t max_rois);

#endif
