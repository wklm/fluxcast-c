#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_mgr = NULL;
static struct wl_output *output = NULL;

static void registry_handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        screencopy_mgr = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!output) {
            output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        }
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void frame_handle_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
        uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    printf("Niri Screencopy Buffer format: 0x%x, size: %ux%u, stride: %u\n", format, width, height, stride);
}

static void frame_handle_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {}
static void frame_handle_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    printf("Frame ready!\n");
}
static void frame_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    printf("Frame failed!\n");
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

int main() {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        printf("Failed to connect to Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!shm || !screencopy_mgr || !output) {
        printf("Missing required Wayland globals: shm=%p, mgr=%p, out=%p\n", (void*)shm, (void*)screencopy_mgr, (void*)output);
        return 1;
    }
    printf("Wayland globals bound successfully!\n");

    struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(screencopy_mgr, 1, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
    wl_display_roundtrip(display);

    zwlr_screencopy_frame_v1_destroy(frame);
    wl_display_disconnect(display);
    return 0;
}
