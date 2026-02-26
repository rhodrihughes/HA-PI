/**
 * light_ui.c — Light tile grid UI for LVGL 9.x
 *
 * Renders a paginated 2×2 grid of light tiles on a 480×320 display.
 * Each tile is a ~220×130px rounded rectangle with label, icon, and
 * state-dependent colour scheme.
 *
 * Horizontal swipe gestures on the screen navigate between pages by
 * animating the page container's x position. Page indicator dots are
 * updated via light_ui_update_page_dots() (weak stub until task 3.6).
 *
 * Uses ONLY LVGL 9.x APIs.
 *
 * Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 4.1, 4.2, 4.3, 4.4
 */

#include "light_ui.h"
#include "display_driver.h"   /* DISP_HOR_RES, DISP_VER_RES */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Layout constants                                                  */
/* ------------------------------------------------------------------ */

#define TILE_WIDTH    220
#define TILE_HEIGHT   130
#define TILE_GAP       10   /* Gap between tiles                      */
#define OUTER_PAD      10   /* Padding around the grid edges          */
#define TILE_RADIUS    12   /* Corner radius for rounded rectangles   */
#define PAGE_WIDTH    DISP_HOR_RES   /* 480px per page                */

/* Grid: 2 columns × 2 rows per page */
#define GRID_COLS      2
#define GRID_ROWS      2

/* ------------------------------------------------------------------ */
/*  Colour definitions                                                */
/* ------------------------------------------------------------------ */

/* ON state: warm amber */
#define COLOR_ON_BG        lv_color_hex(0xFFC864)
#define COLOR_ON_TEXT      lv_color_hex(0x1A1A2E)
#define COLOR_ON_ICON      lv_color_hex(0x1A1A2E)

/* OFF state: dark grey */
#define COLOR_OFF_BG       lv_color_hex(0x2A2A3E)
#define COLOR_OFF_TEXT     lv_color_hex(0x888899)
#define COLOR_OFF_ICON     lv_color_hex(0x555566)

/* UNKNOWN state: muted blue-grey */
#define COLOR_UNKNOWN_BG   lv_color_hex(0x3A3A5C)
#define COLOR_UNKNOWN_TEXT lv_color_hex(0x7777AA)
#define COLOR_UNKNOWN_ICON lv_color_hex(0x6666AA)

/* Screen background */
#define COLOR_SCREEN_BG    lv_color_hex(0x1A1A2E)

/* ------------------------------------------------------------------ */
/*  Module-level state                                                */
/* ------------------------------------------------------------------ */

/** Per-tile LVGL objects */
typedef struct {
    lv_obj_t *tile;       /* The tile container (rounded rect)        */
    lv_obj_t *icon_label; /* Icon label at top of tile                */
    lv_obj_t *name_label; /* Light name label below icon              */
    lv_obj_t *spinner;    /* Spinner for UNKNOWN state (or NULL)      */
} tile_ui_t;

static lv_obj_t       *light_screen = NULL;     /* Dedicated screen       */
static lv_obj_t       *page_container = NULL;    /* Scrollable container   */
static lv_obj_t       *pages[4] = {NULL};        /* Page objects (max 4)   */

static tile_ui_t       tile_objs[LIGHT_MAX_COUNT];
static light_runtime_t tile_runtime[LIGHT_MAX_COUNT];
static light_config_t  tile_config[LIGHT_MAX_COUNT];

static int             light_count = 0;
static int             page_count = 0;
static int             current_page = 0;

static light_toggle_cb_t toggle_cb = NULL;

/** Page indicator dot objects (children of light_screen) */
#define MAX_PAGES          4
#define DOT_SIZE           8
#define DOT_SPACING       16   /* Centre-to-centre distance between dots */
#define DOT_Y_OFFSET      20   /* Distance from bottom of screen         */

static lv_obj_t *dot_objs[MAX_PAGES] = {NULL};

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                  */
/* ------------------------------------------------------------------ */

/* Forward declaration for gesture callback */
static void gesture_event_cb(lv_event_t *e);

/**
 * Animate the page container's x position to show the target page.
 *
 * Slides the wide page_container left/right within the screen viewport
 * so that the target page is visible.
 *
 * @param page  Target page index (must be in [0, page_count-1])
 * @param anim  true for animated transition, false for instant
 */
static void animate_to_page(int page, bool anim)
{
    if (!page_container) return;

    int32_t target_x = -(page * PAGE_WIDTH);

    if (!anim) {
        lv_obj_set_pos(page_container, target_x, 0);
        return;
    }

    /* Use LVGL animation to smoothly slide the container */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, page_container);
    lv_anim_set_values(&a, lv_obj_get_x(page_container), target_x);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_start(&a);
}

/**
 * Gesture event callback for horizontal swipe navigation.
 *
 * Attached to light_screen to detect left/right swipe gestures.
 * Increments or decrements current_page with clamping, then
 * animates the page container to the new position.
 */
static void gesture_event_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    if (dir == LV_DIR_LEFT) {
        /* Swipe left → go to next page */
        if (current_page < page_count - 1) {
            current_page++;
            animate_to_page(current_page, true);
            light_ui_update_page_dots();
        }
    } else if (dir == LV_DIR_RIGHT) {
        /* Swipe right → go to previous page */
        if (current_page > 0) {
            current_page--;
            animate_to_page(current_page, true);
            light_ui_update_page_dots();
        }
    }
}

/**
 * Apply the visual style for a given state to a tile.
 *
 * Sets background colour, text colour, and icon colour based on
 * the light_state_t value. Shows/hides the spinner for UNKNOWN.
 */
static void apply_tile_style(int index, light_state_t state)
{
    if (index < 0 || index >= light_count) return;

    tile_ui_t *t = &tile_objs[index];
    lv_color_t bg, text_col, icon_col;

    switch (state) {
    case LIGHT_STATE_ON:
        bg       = COLOR_ON_BG;
        text_col = COLOR_ON_TEXT;
        icon_col = COLOR_ON_ICON;
        break;
    case LIGHT_STATE_OFF:
        bg       = COLOR_OFF_BG;
        text_col = COLOR_OFF_TEXT;
        icon_col = COLOR_OFF_ICON;
        break;
    case LIGHT_STATE_UNKNOWN:
    default:
        bg       = COLOR_UNKNOWN_BG;
        text_col = COLOR_UNKNOWN_TEXT;
        icon_col = COLOR_UNKNOWN_ICON;
        break;
    }

    /* Tile background */
    lv_obj_set_style_bg_color(t->tile, bg, 0);

    /* Icon colour */
    lv_obj_set_style_text_color(t->icon_label, icon_col, 0);

    /* Label colour */
    lv_obj_set_style_text_color(t->name_label, text_col, 0);

    /* Show spinner only for UNKNOWN state */
    if (t->spinner) {
        if (state == LIGHT_STATE_UNKNOWN) {
            lv_obj_remove_flag(t->spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(t->spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
/**
 * Click event callback for tile tap — optimistic toggle.
 *
 * Extracts the tile index from user_data, flips the displayed state
 * to the opposite value for immediate visual feedback, then invokes
 * the registered toggle callback with the PREVIOUS state so the
 * HA client knows which direction to toggle.
 *
 * Requirements: 5.1, 5.2
 */
static void tile_click_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= light_count) return;

    /* Get current displayed state */
    light_state_t current = tile_runtime[index].optimistic;

    /* Compute opposite: ON→OFF, OFF→ON, UNKNOWN→ON (treat unknown as off) */
    light_state_t next;
    switch (current) {
    case LIGHT_STATE_ON:   next = LIGHT_STATE_OFF; break;
    case LIGHT_STATE_OFF:  next = LIGHT_STATE_ON;  break;
    case LIGHT_STATE_UNKNOWN:
    default:               next = LIGHT_STATE_ON;  break;
    }

    /* Optimistic update — immediate visual feedback */
    tile_runtime[index].optimistic = next;
    apply_tile_style(index, next);

    /* Invoke toggle callback with the state BEFORE the flip */
    if (toggle_cb) {
        toggle_cb(tile_config[index].entity_id, current);
    }
}



/**
 * Create a single tile on a page at the given grid position.
 *
 * @param parent   The page object to add the tile to
 * @param index    Global light index (0-based)
 * @param col      Column in the 2×2 grid (0 or 1)
 * @param row      Row in the 2×2 grid (0 or 1)
 */
static void create_tile(lv_obj_t *parent, int index, int col, int row)
{
    tile_ui_t *t = &tile_objs[index];
    const light_config_t *cfg = &tile_config[index];

    /* Calculate tile position within the page */
    int32_t x = OUTER_PAD + col * (TILE_WIDTH + TILE_GAP);
    int32_t y = OUTER_PAD + row * (TILE_HEIGHT + TILE_GAP);

    /* Create tile container — a styled rounded rectangle */
    t->tile = lv_obj_create(parent);
    lv_obj_set_size(t->tile, TILE_WIDTH, TILE_HEIGHT);
    lv_obj_set_pos(t->tile, x, y);
    lv_obj_remove_flag(t->tile, LV_OBJ_FLAG_SCROLLABLE);

    /* Style: rounded corners, no border */
    lv_obj_set_style_radius(t->tile, TILE_RADIUS, 0);
    lv_obj_set_style_bg_opa(t->tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t->tile, 0, 0);
    lv_obj_set_style_pad_all(t->tile, 10, 0);

    /* Use flex column layout for icon + label */
    lv_obj_set_flex_flow(t->tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t->tile, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t->tile, 8, 0);

    /* Icon label (larger font) */
    t->icon_label = lv_label_create(t->tile);
    lv_label_set_text(t->icon_label, cfg->icon);
    lv_obj_set_style_text_font(t->icon_label, &lv_font_montserrat_32, 0);

    /* Name label (smaller font) */
    t->name_label = lv_label_create(t->tile);
    lv_label_set_text(t->name_label, cfg->label);
    lv_obj_set_style_text_font(t->name_label, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(t->name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t->name_label, TILE_WIDTH - 20);
    lv_obj_set_style_text_align(t->name_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Spinner for UNKNOWN state — small, centred at bottom of tile */
    t->spinner = lv_spinner_create(t->tile);
    lv_obj_set_size(t->spinner, 24, 24);
    lv_spinner_set_anim_params(t->spinner, 1000, 270);
    lv_obj_set_style_arc_color(t->spinner, COLOR_UNKNOWN_ICON, 0);
    lv_obj_set_style_arc_color(t->spinner, lv_color_hex(0x2A2A4E),
                               LV_PART_MAIN);
    /* Start hidden — apply_tile_style will show it for UNKNOWN */
    lv_obj_add_flag(t->spinner, LV_OBJ_FLAG_HIDDEN);

    /* Initialise runtime state */
    tile_runtime[index].state = LIGHT_STATE_UNKNOWN;
    tile_runtime[index].optimistic = LIGHT_STATE_UNKNOWN;
    tile_runtime[index].last_updated_ms = 0;

    /* Apply initial UNKNOWN style */
    apply_tile_style(index, LIGHT_STATE_UNKNOWN);

    /* Register click handler for optimistic toggle (Req 5.1, 5.2) */
    lv_obj_add_flag(t->tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->tile, tile_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
}

/**
 * Create a single page object inside the page container.
 *
 * Each page is exactly DISP_HOR_RES wide and DISP_VER_RES tall,
 * positioned at page_index * PAGE_WIDTH horizontally.
 */
static lv_obj_t *create_page(int page_index)
{
    lv_obj_t *page = lv_obj_create(page_container);
    lv_obj_set_size(page, PAGE_WIDTH, DISP_VER_RES);
    lv_obj_set_pos(page, page_index * PAGE_WIDTH, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* Transparent background — screen bg shows through */
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);

    return page;
}

/**
 * Create page indicator dots at the bottom of the screen.
 *
 * Dots are small circles centred horizontally. The filled/hollow
 * state is set by light_ui_update_page_dots().
 */
static void create_page_dots(void)
{
    /* Total width of all dots with spacing */
    int total_width = page_count * DOT_SIZE + (page_count - 1) * (DOT_SPACING - DOT_SIZE);
    int start_x = (DISP_HOR_RES - total_width) / 2;
    int y = DISP_VER_RES - DOT_Y_OFFSET;

    for (int i = 0; i < page_count && i < MAX_PAGES; i++) {
        dot_objs[i] = lv_obj_create(light_screen);
        lv_obj_set_size(dot_objs[i], DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(dot_objs[i], start_x + i * DOT_SPACING, y);
        lv_obj_remove_flag(dot_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot_objs[i], LV_OBJ_FLAG_CLICKABLE);

        /* Circular shape */
        lv_obj_set_style_radius(dot_objs[i], DOT_SIZE / 2, 0);
        lv_obj_set_style_border_width(dot_objs[i], 0, 0);
        lv_obj_set_style_pad_all(dot_objs[i], 0, 0);
        lv_obj_set_style_bg_opa(dot_objs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot_objs[i], lv_color_white(), 0);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void light_ui_init(const light_config_t *lights, int count)
{
    /* Clamp count to maximum */
    if (count > LIGHT_MAX_COUNT) count = LIGHT_MAX_COUNT;
    if (count < 0) count = 0;

    light_count = count;
    page_count = (count + LIGHT_PER_PAGE - 1) / LIGHT_PER_PAGE;
    if (page_count == 0) page_count = 1;  /* At least one page */
    current_page = 0;

    /* Copy configuration */
    memset(tile_config, 0, sizeof(tile_config));
    if (lights && count > 0) {
        memcpy(tile_config, lights, (size_t)count * sizeof(light_config_t));
    }

    /* Reset tile objects */
    memset(tile_objs, 0, sizeof(tile_objs));
    memset(tile_runtime, 0, sizeof(tile_runtime));

    /* Create a dedicated screen */
    light_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(light_screen, COLOR_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(light_screen, LV_OPA_COVER, 0);

    /* Create the page container — a wide object that holds all pages
     * side by side. Scrolling is disabled by default; swipe navigation
     * (task 3.5) will control scroll position programmatically. */
    page_container = lv_obj_create(light_screen);
    lv_obj_set_size(page_container, page_count * PAGE_WIDTH, DISP_VER_RES);
    lv_obj_set_pos(page_container, 0, 0);
    lv_obj_remove_flag(page_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Transparent, no border/padding — just a positioning container */
    lv_obj_set_style_bg_opa(page_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_container, 0, 0);
    lv_obj_set_style_pad_all(page_container, 0, 0);

    /* Create pages and tiles */
    for (int p = 0; p < page_count; p++) {
        pages[p] = create_page(p);

        /* Create tiles for this page */
        for (int slot = 0; slot < LIGHT_PER_PAGE; slot++) {
            int index = p * LIGHT_PER_PAGE + slot;
            if (index >= light_count) break;

            int col = slot % GRID_COLS;
            int row = slot / GRID_COLS;
            create_tile(pages[p], index, col, row);
        }
    }

    /* Load the light screen */
    lv_scr_load(light_screen);

    /* Register gesture handler on the screen for swipe navigation.
     * We attach to light_screen (the viewport) rather than page_container
     * because the container is wider than the screen. */
    lv_obj_add_event_cb(light_screen, gesture_event_cb,
                        LV_EVENT_GESTURE, NULL);
    /* Clear the gesture on the screen so LVGL doesn't also try to scroll */
    lv_obj_remove_flag(light_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Create page indicator dots and set initial state */
    create_page_dots();
    light_ui_update_page_dots();

    fprintf(stderr, "light_ui_init: %d lights, %d pages\n",
            light_count, page_count);
}

void light_ui_set_state(int index, light_state_t state)
{
    if (index < 0 || index >= light_count) return;

    tile_runtime[index].state = state;
    tile_runtime[index].optimistic = state;
    tile_runtime[index].last_updated_ms = lv_tick_get();

    apply_tile_style(index, state);
}

void light_ui_set_toggle_cb(light_toggle_cb_t cb)
{
    toggle_cb = cb;
}

void light_ui_destroy(void)
{
    if (light_screen) {
        lv_obj_delete(light_screen);
        light_screen = NULL;
    }

    page_container = NULL;
    memset(pages, 0, sizeof(pages));
    memset(dot_objs, 0, sizeof(dot_objs));
    memset(tile_objs, 0, sizeof(tile_objs));
    memset(tile_runtime, 0, sizeof(tile_runtime));
    memset(tile_config, 0, sizeof(tile_config));

    light_count = 0;
    page_count = 0;
    current_page = 0;
    toggle_cb = NULL;
}

int light_ui_get_page_count(void)
{
    return page_count;
}

int light_ui_get_current_page(void)
{
    return current_page;
}

const light_runtime_t *light_ui_get_runtime(int index)
{
    if (index < 0 || index >= light_count) return NULL;
    return &tile_runtime[index];
}

lv_obj_t *light_ui_get_tile_obj(int index)
{
    if (index < 0 || index >= light_count) return NULL;
    return tile_objs[index].tile;
}

lv_obj_t *light_ui_get_container(void)
{
    return page_container;
}

void light_ui_set_page(int page)
{
    /* Clamp to valid range */
    if (page < 0) page = 0;
    if (page >= page_count) page = page_count - 1;

    current_page = page;
    animate_to_page(current_page, true);
    light_ui_update_page_dots();
}

/**
 * Update page indicator dots to reflect the current page.
 *
 * Filled (full opacity) for the current page, hollow (low opacity)
 * for all other pages.
 */
void light_ui_update_page_dots(void)
{
    for (int i = 0; i < page_count && i < MAX_PAGES; i++) {
        if (!dot_objs[i]) continue;

        if (i == current_page) {
            /* Filled dot — full opacity white */
            lv_obj_set_style_bg_opa(dot_objs[i], LV_OPA_COVER, 0);
        } else {
            /* Hollow dot — low opacity */
            lv_obj_set_style_bg_opa(dot_objs[i], LV_OPA_30, 0);
        }
    }
}
