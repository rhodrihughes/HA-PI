/**
 * main.c — Application entry point for HA Light Control
 *
 * Initialises LVGL, display/touch drivers, loads config, starts the
 * HA client and web config server, then runs the LVGL main loop at
 * ~30 fps with periodic HA state polling every 5 seconds.
 *
 * Handles SIGINT/SIGTERM for clean shutdown.
 *
 * Requirements: 12.1, 12.2, 12.3, 6.1
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "config.h"
#include "config_server.h"
#include "display_driver.h"
#include "ha_client.h"
#include "light_ui.h"
#include "touch_driver.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define DEFAULT_CONFIG_PATH  "/etc/ha_lights.conf"
#define WEB_SERVER_PORT      8080
#define POLL_INTERVAL_MS     5000   /* 5 seconds */
#define FRAME_PERIOD_MS      33     /* ~30 fps   */

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_shutdown = 0;
static config_t              g_config;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/** Monotonic millisecond clock for LVGL tick and frame pacing. */
static uint32_t get_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/** SIGINT / SIGTERM handler — sets shutdown flag. */
static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/** Toggle callback wired to Light_UI tile taps. */
static void on_light_toggle(const char *entity_id, light_state_t current_state)
{
    (void)current_state;
    ha_toggle_light(entity_id);
}

/** lv_timer callback for periodic HA state polling. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ha_poll_all(g_config.lights, g_config.light_count);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *config_path = DEFAULT_CONFIG_PATH;

    /* Allow overriding config path via CLI argument */
    if (argc > 1)
        config_path = argv[1];

    /* --- Signal handling ------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* --- LVGL init ------------------------------------------------ */
    lv_init();

    /* Register tick provider — LVGL 9.x needs this to track time
     * for input handling, animations, and timer scheduling. */
    lv_tick_set_cb(get_tick_ms);

    /* --- Hardware drivers ----------------------------------------- */
    if (display_driver_init() != 0) {
        fprintf(stderr, "main: display_driver_init failed\n");
        return EXIT_FAILURE;
    }
    if (touch_driver_init() != 0) {
        fprintf(stderr, "main: touch_driver_init failed\n");
        display_driver_deinit();
        return EXIT_FAILURE;
    }

    /* --- Configuration -------------------------------------------- */
    if (config_load(config_path, &g_config) != 0) {
        fprintf(stderr, "main: config_load failed for %s\n", config_path);
        touch_driver_deinit();
        display_driver_deinit();
        return EXIT_FAILURE;
    }
    config_set_path(config_path);

    /* --- Light UI ------------------------------------------------- */
    light_ui_init(g_config.lights, g_config.light_count);
    light_ui_set_toggle_cb(on_light_toggle);

    /* --- HA client ------------------------------------------------ */
    int ha_ok = 0;
    if (g_config.ha.base_url[0] != '\0' && g_config.ha.token[0] != '\0') {
        if (ha_client_init(g_config.ha.base_url, g_config.ha.token) == 0) {
            ha_ok = 1;
            /* Initial state fetch */
            ha_poll_all(g_config.lights, g_config.light_count);
        } else {
            fprintf(stderr, "main: ha_client_init failed (non-fatal)\n");
        }
    } else {
        fprintf(stderr, "main: HA credentials not configured — "
                "UI will show, use web config at :8080 to set up\n");
    }

    /* Periodic polling timer (every 5 s) — only if HA client is up */
    if (ha_ok) {
        lv_timer_create(poll_timer_cb, POLL_INTERVAL_MS, NULL);
    }

    /* --- Web config server ---------------------------------------- */
    config_server_set_path(config_path);
    if (config_server_start(WEB_SERVER_PORT, &g_config) != 0) {
        fprintf(stderr, "main: config_server_start failed (non-fatal)\n");
        /* Non-fatal — the display app still works without the web UI */
    }

    /* --- Main loop (~30 fps) -------------------------------------- */
    fprintf(stdout, "ha-pi: running (config=%s)\n", config_path);

    while (!g_shutdown) {
        uint32_t t0 = get_tick_ms();
        lv_timer_handler();
        uint32_t elapsed = get_tick_ms() - t0;

        if (elapsed < FRAME_PERIOD_MS)
            usleep((FRAME_PERIOD_MS - elapsed) * 1000);
    }

    /* --- Clean shutdown ------------------------------------------- */
    fprintf(stdout, "ha-pi: shutting down\n");

    config_server_stop();
    ha_client_cleanup();
    light_ui_destroy();
    touch_driver_deinit();
    display_driver_deinit();
    lv_deinit();

    return EXIT_SUCCESS;
}
