/**
 * light_ui.h — Light tile grid UI for LVGL 9.x
 *
 * Renders a paginated 2×2 grid of light tiles on a 480×320 display.
 * Each tile shows a light's label, icon, and ON/OFF/UNKNOWN state
 * with corresponding colour scheme.
 *
 * Uses ONLY LVGL 9.x APIs.
 *
 * Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6
 */

#ifndef LIGHT_UI_H
#define LIGHT_UI_H

#include "lvgl.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define LIGHT_MAX_COUNT   16   /* Maximum number of lights (4 pages)  */
#define LIGHT_PER_PAGE     4   /* 2×2 grid per page                   */

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

/** Static configuration for a single light (loaded from config file). */
typedef struct {
    char entity_id[64];   /* HA entity ID, e.g. "light.kitchen"       */
    char label[32];       /* Display name shown on tile                */
    char icon[8];         /* UTF-8 emoji or LV symbol                  */
} light_config_t;

/** Possible states for a light tile. */
typedef enum {
    LIGHT_STATE_UNKNOWN = 0,
    LIGHT_STATE_OFF,
    LIGHT_STATE_ON,
} light_state_t;

/** Runtime state tracked per light tile. */
typedef struct {
    light_state_t state;          /* Current confirmed state           */
    light_state_t optimistic;     /* State shown after tap, pre-confirm*/
    uint32_t      last_updated_ms;/* Timestamp of last successful poll */
} light_runtime_t;

/** Callback invoked when a tile is tapped. */
typedef void (*light_toggle_cb_t)(const char *entity_id,
                                   light_state_t current_state);

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * Initialise the light UI with a list of lights.
 *
 * Creates a full-screen container with horizontally arranged pages,
 * each holding a 2×2 grid of tiles. All tiles start in UNKNOWN state.
 *
 * @param lights  Array of light configurations
 * @param count   Number of lights (clamped to LIGHT_MAX_COUNT)
 */
void light_ui_init(const light_config_t *lights, int count);

/**
 * Update a tile's visual state.
 *
 * Called from the HA poll callback to reconcile tile appearance
 * with the confirmed state from Home Assistant.
 *
 * @param index  Tile index (0-based)
 * @param state  New confirmed state
 */
void light_ui_set_state(int index, light_state_t state);

/**
 * Register a callback invoked when the user taps a tile.
 *
 * @param cb  Toggle callback function
 */
void light_ui_set_toggle_cb(light_toggle_cb_t cb);

/**
 * Destroy the light UI and free all resources.
 *
 * Removes the screen object and resets internal state.
 */
void light_ui_destroy(void);

/**
 * Get the number of pages created by light_ui_init.
 *
 * @return Number of pages, or 0 if UI is not initialised
 */
int light_ui_get_page_count(void);

/**
 * Get the current page index.
 *
 * @return Current page index (0-based)
 */
int light_ui_get_current_page(void);

/**
 * Get the runtime state for a tile.
 *
 * @param index  Tile index (0-based)
 * @return Pointer to runtime state, or NULL if index is out of range
 */
const light_runtime_t *light_ui_get_runtime(int index);

/**
 * Get the LVGL object for a tile.
 *
 * @param index  Tile index (0-based)
 * @return Pointer to the tile's lv_obj, or NULL if index is out of range
 */
lv_obj_t *light_ui_get_tile_obj(int index);

/**
 * Get the page container object.
 *
 * @return Pointer to the scrollable page container, or NULL if not initialised
 */
lv_obj_t *light_ui_get_container(void);

/**
 * Navigate to a specific page with animation.
 *
 * Clamps the page index to [0, page_count - 1]. Animates the
 * page container to show the target page and updates indicator dots.
 *
 * @param page  Target page index (0-based)
 */
void light_ui_set_page(int page);

/**
 * Update page indicator dots to reflect the current page.
 *
 * Sets the dot at current_page to filled (full opacity) and all
 * other dots to hollow (low opacity). Called automatically on
 * page changes and during initialisation.
 */
void light_ui_update_page_dots(void);

#endif /* LIGHT_UI_H */
