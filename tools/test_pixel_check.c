#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_mgr = NULL;
static struct wl_output *output = NULL;
static struct wl_buffer *wl_buf = NULL;
static uint32_t *shm_data = NULL;
static int shm_fd = -1;
static volatile int ready = 0;

static void registry_handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, wl_shm_interface.name) == 0) shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) screencopy_mgr = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 1);
    else if (strcmp(interface, wl_output_interface.name) == 0) if (!output) output = wl_registry_bind(registry, name, &wl_output_interface, 1);
}
static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { .global = registry_handle_global, .global_remove = registry_handle_global_remove };

static void frame_handle_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
        uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    if (!wl_buf) {
        size_t size = stride * height;
        shm_fd = memfd_create("test-shm", MFD_CLOEXEC);
        ftruncate(shm_fd, size);
        shm_data = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, shm_fd, size);
        wl_buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
        wl_shm_pool_destroy(pool);
    }
    zwlr_screencopy_frame_v1_copy(frame, wl_buf);
}
static void frame_handle_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {}
static void frame_handle_ready(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t a, uint32_t b, uint32_t c) { ready = 1; }
static void frame_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) { ready = -1; }
static const struct zwlr_screencopy_frame_v1_listener f_listener = {
    .buffer = frame_handle_buffer, .flags = frame_handle_flags, .ready = frame_handle_ready, .failed = frame_handle_failed
};

int main() {
    struct wl_display *d = wl_display_connect(NULL);
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &reg_listener, NULL);
    wl_display_roundtrip(d);

    ready = 0;
    struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output(screencopy_mgr, 1, output);
    zwlr_screencopy_frame_v1_add_listener(f, &f_listener, NULL);
    wl_display_flush(d);

    while (!ready) {
        wl_display_dispatch(d);
    }
    zwlr_screencopy_frame_v1_destroy(f);

    int nonzero = 0;
    for (int i = 0; i < 1920 * 1080; i++) {
        if (shm_data[i] != 0) nonzero++;
    }
    printf("Nonzero pixels in captured buffer: %d / %d (first pixel: 0x%08x)\n", nonzero, 1920*1080, shm_data[0]);

    wl_display_disconnect(d);
    return 0;
}
