/**
 * touch_driver.c — XPT2046 SPI touchscreen driver for LVGL 9.x
 *
 * Clean LVGL 9.x implementation from scratch. No v8 API usage.
 *
 * Hardware:
 *   SPI  : /dev/spidev0.1 at 1 MHz
 *   IRQ  : GPIO 24 (active low — low when pen is down)
 *
 * The XPT2046 is a 12-bit resistive touch controller. Raw ADC values
 * are mapped to screen coordinates using calibration constants that
 * account for the specific panel mounting orientation.
 *
 * A background pthread polls the controller at 50 Hz and stores the
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
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

/* ------------------------------------------------------------------ */
/*  Hardware / SPI configuration                                      */
/* ------------------------------------------------------------------ */

#define TOUCH_SPI_DEVICE   "/dev/spidev0.1"
#define TOUCH_SPI_SPEED_HZ 1000000   /* 1 MHz */
#define TOUCH_SPI_MODE     SPI_MODE_0
#define TOUCH_SPI_BPW      8         /* bits per word */

#define GPIO_TOUCH_IRQ 24  /* Active low — low when pen is down */

/* ------------------------------------------------------------------ */
/*  XPT2046 command bytes                                             */
/* ------------------------------------------------------------------ */

/*
 * XPT2046 control byte format:
 *   Bit 7   : S (start) = 1
 *   Bit 6-4 : A2-A0 channel select
 *   Bit 3   : MODE (0 = 12-bit, 1 = 8-bit)
 *   Bit 2   : SER/DFR (0 = differential, 1 = single-ended)
 *   Bit 1-0 : PD1-PD0 power-down mode
 *
 * We use 12-bit differential mode with power-down between conversions.
 */
#define XPT2046_CMD_X  0xD0  /* S=1, A2A1A0=101 (X), 12-bit, DFR, PD=00 */
#define XPT2046_CMD_Y  0x90  /* S=1, A2A1A0=001 (Y), 12-bit, DFR, PD=00 */

/* ------------------------------------------------------------------ */
/*  Calibration constants                                             */
/* ------------------------------------------------------------------ */

/*
 * Raw ADC range observed for the XPT2046 on a typical 480×320 panel.
 * These map the 12-bit ADC readings to screen pixel coordinates.
 *
 * Adjust these values if taps register in the wrong position:
 *   TOUCH_X_MIN / MAX : raw ADC range for the horizontal axis
 *   TOUCH_Y_MIN / MAX : raw ADC range for the vertical axis
 *
 * The mapping is linear:
 *   screen_x = (raw_x - X_MIN) * DISP_HOR_RES / (X_MAX - X_MIN)
 *   screen_y = (raw_y - Y_MIN) * DISP_VER_RES / (Y_MAX - Y_MIN)
 */
#define TOUCH_X_MIN  200
#define TOUCH_X_MAX  3900
#define TOUCH_Y_MIN  200
#define TOUCH_Y_MAX  3900

/* Number of samples to average for noise rejection */
#define TOUCH_SAMPLES 4

/* ------------------------------------------------------------------ */
/*  Polling configuration                                             */
/* ------------------------------------------------------------------ */

#define TOUCH_POLL_HZ       50
#define TOUCH_POLL_INTERVAL_US (1000000 / TOUCH_POLL_HZ)  /* 20 ms */

/* ------------------------------------------------------------------ */
/*  Shared touch state (protected by mutex)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t  x;        /* Screen X coordinate (pixels) */
    int16_t  y;        /* Screen Y coordinate (pixels) */
    bool     pressed;  /* true if pen is currently down */
} touch_state_t;

static pthread_mutex_t touch_mutex = PTHREAD_MUTEX_INITIALIZER;
static touch_state_t   touch_state = { .x = 0, .y = 0, .pressed = false };

/* ------------------------------------------------------------------ */
/*  Module-level state                                                */
/* ------------------------------------------------------------------ */

static int spi_fd = -1;              /* SPI file descriptor           */
static int gpio_irq_fd = -1;        /* GPIO value fd for IRQ pin     */
static lv_indev_t *indev = NULL;    /* LVGL input device handle      */

static pthread_t poll_thread;        /* Background polling thread     */
static volatile bool poll_running = false;  /* Thread run flag        */

/* ------------------------------------------------------------------ */
/*  GPIO helpers (sysfs interface)                                    */
/* ------------------------------------------------------------------ */

/**
 * Export a GPIO pin and open its value file for reading.
 * Returns an open fd, or -1 on error.
 */
static int gpio_export_input(int pin)
{
    char path[64];
    int fd;

    /* Export the pin (ignore EBUSY — already exported) */
    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d", pin);
        if (write(fd, buf, len) < 0 && errno != EBUSY) {
            perror("gpio export write");
        }
        close(fd);
    }

    /* Brief delay for sysfs to create the pin directory */
    usleep(100000);

    /* Set direction to input */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "gpio_export_input: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (write(fd, "in", 2) < 0) {
        perror("gpio direction write");
        close(fd);
        return -1;
    }
    close(fd);

    /* Open the value file for reading */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "gpio_export_input: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    return fd;
}

/** Read the current value of a GPIO input pin. Returns 0 or 1. */
static int gpio_read(int fd)
{
    char buf[4] = {0};
    lseek(fd, 0, SEEK_SET);
    if (read(fd, buf, sizeof(buf) - 1) < 0) {
        perror("gpio_read");
        return 1;  /* Default to "not pressed" (IRQ is active low) */
    }
    return (buf[0] == '1') ? 1 : 0;
}

/** Unexport a GPIO pin and close its fd. */
static void gpio_unexport(int pin, int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    int exp_fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (exp_fd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d", pin);
        if (write(exp_fd, buf, len) < 0) {
            /* Ignore — pin may not be exported */
        }
        close(exp_fd);
    }
}

/* ------------------------------------------------------------------ */
/*  SPI helpers                                                       */
/* ------------------------------------------------------------------ */

/** Open and configure the SPI device for XPT2046. Returns 0 on success. */
static int touch_spi_init(void)
{
    spi_fd = open(TOUCH_SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        fprintf(stderr, "touch_spi_init: cannot open %s: %s\n",
                TOUCH_SPI_DEVICE, strerror(errno));
        return -1;
    }

    uint8_t mode  = TOUCH_SPI_MODE;
    uint8_t bpw   = TOUCH_SPI_BPW;
    uint32_t speed = TOUCH_SPI_SPEED_HZ;

    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("touch_spi_init: SPI_IOC_WR_MODE");
        return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0) {
        perror("touch_spi_init: SPI_IOC_WR_BITS_PER_WORD");
        return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("touch_spi_init: SPI_IOC_WR_MAX_SPEED_HZ");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  XPT2046 reading                                                   */
/* ------------------------------------------------------------------ */

/**
 * Read a single 12-bit ADC value from the XPT2046.
 *
 * Sends a 3-byte SPI transaction:
 *   Byte 0: command byte (TX)
 *   Byte 1: high bits of result (RX)
 *   Byte 2: low bits of result (RX)
 *
 * The 12-bit result is in bits [14:3] of the 16-bit response.
 */
static uint16_t xpt2046_read_channel(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };

    struct spi_ioc_transfer tr = {
        .tx_buf        = (unsigned long)tx,
        .rx_buf        = (unsigned long)rx,
        .len           = 3,
        .speed_hz      = TOUCH_SPI_SPEED_HZ,
        .bits_per_word = TOUCH_SPI_BPW,
        .delay_usecs   = 0,
    };

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        perror("xpt2046_read_channel");
        return 0;
    }

    /* 12-bit result: rx[1] bits [6:0] are D11..D5, rx[2] bits [7:3] are D4..D0 */
    uint16_t raw = (uint16_t)((rx[1] << 8) | rx[2]) >> 3;
    return raw & 0x0FFF;
}

/**
 * Read averaged X and Y coordinates from the XPT2046.
 *
 * Takes TOUCH_SAMPLES readings and averages them to reduce noise
 * from the resistive panel. Discards the first reading (settling time).
 *
 * @param[out] raw_x  Averaged raw X ADC value
 * @param[out] raw_y  Averaged raw Y ADC value
 */
static void xpt2046_read_xy(uint16_t *raw_x, uint16_t *raw_y)
{
    uint32_t sum_x = 0;
    uint32_t sum_y = 0;

    /* Discard first reading for settling */
    (void)xpt2046_read_channel(XPT2046_CMD_X);
    (void)xpt2046_read_channel(XPT2046_CMD_Y);

    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        sum_x += xpt2046_read_channel(XPT2046_CMD_X);
        sum_y += xpt2046_read_channel(XPT2046_CMD_Y);
    }

    *raw_x = (uint16_t)(sum_x / TOUCH_SAMPLES);
    *raw_y = (uint16_t)(sum_y / TOUCH_SAMPLES);
}

/**
 * Map a raw ADC value to a screen pixel coordinate.
 *
 * Clamps the result to [0, max_px - 1] to prevent out-of-bounds values.
 */
static int16_t map_raw_to_screen(uint16_t raw, uint16_t raw_min,
                                  uint16_t raw_max, int16_t max_px)
{
    if (raw <= raw_min) return 0;
    if (raw >= raw_max) return max_px - 1;

    int32_t mapped = (int32_t)(raw - raw_min) * max_px / (raw_max - raw_min);

    if (mapped < 0) return 0;
    if (mapped >= max_px) return max_px - 1;
    return (int16_t)mapped;
}

/* ------------------------------------------------------------------ */
/*  Background polling thread                                         */
/* ------------------------------------------------------------------ */

/**
 * Polling thread entry point.
 *
 * Runs at 50 Hz, reading the XPT2046 and updating the shared touch
 * state behind the mutex. The IRQ pin is checked first as a quick
 * "pen down" indicator — if the pen is up, we skip the SPI read.
 *
 * Requirement 2.2: poll at 50 Hz in a dedicated background pthread.
 */
static void *touch_poll_thread(void *arg)
{
    (void)arg;

    while (poll_running) {
        bool pressed = false;
        int16_t sx = 0;
        int16_t sy = 0;

        /*
         * Check IRQ pin: active low means pen is down.
         * If the GPIO fd is not available, fall back to always reading
         * the SPI (less efficient but still functional).
         */
        bool pen_down = true;
        if (gpio_irq_fd >= 0) {
            pen_down = (gpio_read(gpio_irq_fd) == 0);
        }

        if (pen_down) {
            uint16_t raw_x, raw_y;
            xpt2046_read_xy(&raw_x, &raw_y);

            /* Only treat as a valid touch if readings are in a sane range */
            if (raw_x > TOUCH_X_MIN && raw_x < TOUCH_X_MAX &&
                raw_y > TOUCH_Y_MIN && raw_y < TOUCH_Y_MAX) {
                sx = map_raw_to_screen(raw_x, TOUCH_X_MIN, TOUCH_X_MAX,
                                       DISP_HOR_RES);
                sy = map_raw_to_screen(raw_y, TOUCH_Y_MIN, TOUCH_Y_MAX,
                                       DISP_VER_RES);
                pressed = true;
            }
        }

        /* Update shared state under lock */
        pthread_mutex_lock(&touch_mutex);
        touch_state.pressed = pressed;
        if (pressed) {
            touch_state.x = sx;
            touch_state.y = sy;
        }
        pthread_mutex_unlock(&touch_mutex);

        usleep(TOUCH_POLL_INTERVAL_US);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  LVGL input device read callback                                   */
/* ------------------------------------------------------------------ */

/**
 * LVGL 9.x input device read callback.
 *
 * Called by LVGL on each input tick to get the current touch state.
 * Reads the shared state (set by the polling thread) under the mutex.
 *
 * Requirement 2.3: registered via lv_indev_create() + lv_indev_set_type().
 * Requirement 2.4: uses ONLY LVGL 9.x API.
 */
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

/**
 * Initialise the XPT2046 touchscreen and register with LVGL 9.x.
 *
 * Requirement 2.1: opens /dev/spidev0.1 at 1 MHz
 * Requirement 2.2: polls at 50 Hz in a background pthread
 * Requirement 2.3: registers via lv_indev_create() + lv_indev_set_type()
 * Requirement 2.4: no v8 API usage
 */
int touch_driver_init(void)
{
    /* --- SPI setup ------------------------------------------------ */
    if (touch_spi_init() != 0) {
        fprintf(stderr, "touch_driver_init: SPI init failed\n");
        return -1;
    }

    /* --- GPIO IRQ pin (optional — graceful fallback if unavailable) */
    gpio_irq_fd = gpio_export_input(GPIO_TOUCH_IRQ);
    if (gpio_irq_fd < 0) {
        fprintf(stderr, "touch_driver_init: IRQ GPIO not available, "
                "falling back to continuous SPI polling\n");
        /* Not fatal — we can still poll without the IRQ pin */
    }

    /* --- Start polling thread ------------------------------------- */
    poll_running = true;
    if (pthread_create(&poll_thread, NULL, touch_poll_thread, NULL) != 0) {
        fprintf(stderr, "touch_driver_init: failed to create poll thread\n");
        touch_driver_deinit();
        return -1;
    }

    /* --- LVGL input device registration (9.x API only) ------------ */

    /* Create input device — LVGL 9.x API (not lv_indev_drv_init/register) */
    indev = lv_indev_create();
    if (!indev) {
        fprintf(stderr, "touch_driver_init: lv_indev_create failed\n");
        touch_driver_deinit();
        return -1;
    }

    /* Set type to pointer — LVGL 9.x API */
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    /* Set read callback — LVGL 9.x API */
    lv_indev_set_read_cb(indev, touch_read_cb);

    fprintf(stderr, "touch_driver_init: XPT2046 ready, polling at %d Hz\n",
            TOUCH_POLL_HZ);
    return 0;
}

void touch_driver_deinit(void)
{
    /* Stop polling thread */
    if (poll_running) {
        poll_running = false;
        pthread_join(poll_thread, NULL);
    }

    /* Close SPI */
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }

    /* Unexport IRQ GPIO */
    gpio_unexport(GPIO_TOUCH_IRQ, &gpio_irq_fd);

    /* LVGL input device handle is managed by LVGL — don't free manually */
    indev = NULL;
}
