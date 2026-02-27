/**
 * touch_driver.c — Touchscreen driver for LVGL 9.x
 *
 * Reads touch events from the Linux input subsystem (/dev/input/eventX).
 * When the LCD-show kernel driver is installed, the XPT2046 appears as
 * a standard input device with ABS_X/ABS_Y events.
 *
 * Falls back to scanning all /dev/input/event* devices to find one
 * that reports ABS_X capability (i.e. a touchscreen).
 *
 * A background pthread reads events at native rate and stores the
 * latest touch state behind a mutex. LVGL's read callback picks up
 * the state on each input tick.
 *
 * Requirements: 2.1, 2.2, 2.3, 2.4
 */

#include "touch_driver.h"
#include "display_driver.h"   /* DISP_HOR_RES, DISP_VER_RES */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/ioctl.h>

/* ------------------------------------------------------------------ */
/*  Shared touch state (protected by mutex)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t  x;
    int16_t  y;
    bool     pressed;
} touch_state_t;

static pthread_mutex_t touch_mutex = PTHREAD_MUTEX_INITIALIZER;
static touch_state_t   touch_state = { .x = 0, .y = 0, .pressed = false };

/* ------------------------------------------------------------------ */
/*  Module-level state                                                */
/* ------------------------------------------------------------------ */

static int event_fd = -1;
static lv_indev_t *indev = NULL;
static pthread_t poll_thread;
static volatile bool poll_running = false;

/* ABS axis ranges from the kernel driver */
static int32_t abs_x_min = 0, abs_x_max = 4095;
static int32_t abs_y_min = 0, abs_y_max = 4095;

/* ------------------------------------------------------------------ */
/*  Input device discovery                                            */
/* ------------------------------------------------------------------ */

/**
 * Check if a /dev/input/eventX device has ABS_X capability
 * (i.e. is a touchscreen or similar absolute pointing device).
 */
static int has_abs_x(int fd)
{
    unsigned long abs_bits[(ABS_MAX + 8 * sizeof(unsigned long) - 1)
                           / (8 * sizeof(unsigned long))] = {0};

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return 0;

    /* Check if ABS_X bit is set */
    return (abs_bits[ABS_X / (8 * sizeof(unsigned long))]
            >> (ABS_X % (8 * sizeof(unsigned long)))) & 1;
}

/**
 * Find and open the first touchscreen input device.
 * Returns fd on success, -1 if none found.
 */
static int find_touch_device(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *ent;
    int fd = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int test_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (test_fd < 0) continue;

        if (has_abs_x(test_fd)) {
            /* Read axis ranges */
            struct input_absinfo abs_info;
            if (ioctl(test_fd, EVIOCGABS(ABS_X), &abs_info) == 0) {
                abs_x_min = abs_info.minimum;
                abs_x_max = abs_info.maximum;
            }
            if (ioctl(test_fd, EVIOCGABS(ABS_Y), &abs_info) == 0) {
                abs_y_min = abs_info.minimum;
                abs_y_max = abs_info.maximum;
            }

            char name[128] = "Unknown";
            ioctl(test_fd, EVIOCGNAME(sizeof(name)), name);
            fprintf(stderr, "touch_driver: found '%s' at %s "
                    "(X: %d-%d, Y: %d-%d)\n",
                    name, path, abs_x_min, abs_x_max,
                    abs_y_min, abs_y_max);

            fd = test_fd;
            break;
        }
        close(test_fd);
    }

    closedir(dir);
    return fd;
}

/* ------------------------------------------------------------------ */
/*  Coordinate mapping                                                */
/* ------------------------------------------------------------------ */

static int16_t map_axis(int32_t raw, int32_t raw_min, int32_t raw_max,
                         int16_t screen_max)
{
    if (raw_max <= raw_min) return 0;
    int32_t mapped = (raw - raw_min) * screen_max / (raw_max - raw_min);
    if (mapped < 0) return 0;
    if (mapped >= screen_max) return screen_max - 1;
    return (int16_t)mapped;
}

/* ------------------------------------------------------------------ */
/*  Background event reading thread                                   */
/* ------------------------------------------------------------------ */

static void *touch_poll_thread_fn(void *arg)
{
    (void)arg;
    struct input_event ev;
    int32_t raw_x = 0, raw_y = 0;
    bool pressed = false;

    /* Grab exclusive access so no other process consumes our events */
    if (ioctl(event_fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "touch_driver: EVIOCGRAB failed (non-fatal): %s\n",
                strerror(errno));
    }

    while (poll_running) {
        ssize_t n = read(event_fd, &ev, sizeof(ev));
        if (n < (ssize_t)sizeof(ev)) {
            usleep(5000);
            continue;
        }

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X)
                raw_x = ev.value;
            else if (ev.code == ABS_Y)
                raw_y = ev.value;
            else if (ev.code == ABS_PRESSURE)
                pressed = (ev.value > 0);
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            pressed = (ev.value != 0);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            /* XPT2046 touch digitizer axes are rotated relative to the
             * ILI9486 LCD in landscape (480×320) mode:
             *   - Touch ABS_X maps to screen Y (inverted)
             *   - Touch ABS_Y maps to screen X
             * Swap and invert to get correct screen coordinates. */
            int16_t sx = map_axis(raw_y, abs_y_min, abs_y_max, DISP_HOR_RES);
            int16_t sy = (DISP_VER_RES - 1) -
                         map_axis(raw_x, abs_x_min, abs_x_max, DISP_VER_RES);

            pthread_mutex_lock(&touch_mutex);
            touch_state.pressed = pressed;
            if (pressed) {
                touch_state.x = sx;
                touch_state.y = sy;
            }
            pthread_mutex_unlock(&touch_mutex);

            fprintf(stderr, "touch: %s x=%d y=%d (raw %d,%d)\n",
                    pressed ? "DOWN" : "UP  ", sx, sy, raw_x, raw_y);
        }
    }

    /* Release grab on exit */
    ioctl(event_fd, EVIOCGRAB, 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  LVGL input device read callback                                   */
/* ------------------------------------------------------------------ */

static void touch_read_cb(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;

    pthread_mutex_lock(&touch_mutex);
    data->point.x = touch_state.x;
    data->point.y = touch_state.y;
    data->state   = touch_state.pressed ? LV_INDEV_STATE_PRESSED
                                        : LV_INDEV_STATE_RELEASED;
    pthread_mutex_unlock(&touch_mutex);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int touch_driver_init(void)
{
    /* Find a touchscreen input device */
    event_fd = find_touch_device();
    if (event_fd < 0) {
        fprintf(stderr, "touch_driver_init: no touchscreen found in /dev/input/\n");
        return -1;
    }

    /* Start event reading thread */
    poll_running = true;
    if (pthread_create(&poll_thread, NULL, touch_poll_thread_fn, NULL) != 0) {
        fprintf(stderr, "touch_driver_init: failed to create event thread\n");
        close(event_fd);
        event_fd = -1;
        return -1;
    }

    /* Register with LVGL 9.x */
    indev = lv_indev_create();
    if (!indev) {
        fprintf(stderr, "touch_driver_init: lv_indev_create failed\n");
        touch_driver_deinit();
        return -1;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    fprintf(stderr, "touch_driver_init: using Linux input subsystem\n");
    return 0;
}

void touch_driver_deinit(void)
{
    if (poll_running) {
        poll_running = false;
        pthread_join(poll_thread, NULL);
    }

    if (event_fd >= 0) {
        close(event_fd);
        event_fd = -1;
    }

    indev = NULL;
}
