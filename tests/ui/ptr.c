// Test tool: real pointer input through the compositor (wlroots virtual pointer), so the
// panel and the bar button can be clicked for real rather than poked from inside.
//   ptr W H move X Y | click X Y | drag X1 Y1 X2 Y2 | scroll X Y AMOUNT     (logical pixels)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "vp.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(interface, "zwlr_virtual_pointer_manager_v1"))
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void gone(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener listener = {global, gone};

static uint32_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: ptr W H move|click|drag|scroll X Y [...]\n"); return 2; }
    int W = atoi(argv[1]), H = atoi(argv[2]);
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "no Wayland display\n"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    wl_display_roundtrip(display);
    if (!manager) { fprintf(stderr, "the compositor has no virtual pointer\n"); return 1; }
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
#define MOVE(x, y) do { zwlr_virtual_pointer_v1_motion_absolute(p, now(), (x), (y), W, H); zwlr_virtual_pointer_v1_frame(p); wl_display_roundtrip(display); usleep(15000); } while (0)
#define BUTTON(state) do { zwlr_virtual_pointer_v1_button(p, now(), 0x110, (state)); zwlr_virtual_pointer_v1_frame(p); wl_display_roundtrip(display); usleep(60000); } while (0)
    int x = atoi(argv[4]), y = atoi(argv[5]);
    MOVE(x - 3, y - 2); MOVE(x, y); usleep(150000);
    if (!strcmp(argv[3], "click")) { BUTTON(1); BUTTON(0); }
    else if (!strcmp(argv[3], "drag") && argc >= 8) {
        int x2 = atoi(argv[6]), y2 = atoi(argv[7]);
        BUTTON(1);
        for (int i = 1; i <= 20; i++) MOVE(x + (x2 - x) * i / 20, y + (y2 - y) * i / 20);
        usleep(100000); BUTTON(0);
    } else if (!strcmp(argv[3], "scroll") && argc >= 7) {
        for (int i = 0; i < 6; i++) {
            zwlr_virtual_pointer_v1_axis_source(p, 0);
            zwlr_virtual_pointer_v1_axis(p, now(), 0, wl_fixed_from_int(atoi(argv[6]) / 6));
            zwlr_virtual_pointer_v1_frame(p); wl_display_roundtrip(display); usleep(30000);
        }
    }
    usleep(100000);
    zwlr_virtual_pointer_v1_destroy(p); wl_display_roundtrip(display);
    return 0;
}
