/**
 * config.h — Configuration management for HA Light Control
 *
 * Loads, validates, saves, and hot-reloads a JSON configuration file
 * containing Home Assistant connection settings, web password hash,
 * and the list of controllable lights.
 *
 * Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "light_ui.h"
#include "ha_client.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define CONFIG_MAX_LIGHTS     16
#define CONFIG_PATH_MAX      256
#define CONFIG_WEB_HASH_MAX  128

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

/** Full application configuration. */
typedef struct {
    ha_config_t    ha;                              /* HA URL + token       */
    char           web_password_hash[CONFIG_WEB_HASH_MAX]; /* bcrypt hash  */
    light_config_t lights[CONFIG_MAX_LIGHTS];       /* Light definitions    */
    int            light_count;                     /* Number of lights     */
} config_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * Load and validate configuration from a JSON file.
 *
 * Validates:
 *   - Each entity_id is non-empty and matches <domain>.<name> format
 *   - Each label is non-empty and ≤ 31 characters
 *   - Light count ≤ 16
 *
 * @param path  Path to JSON config file
 * @param out   Destination config struct
 * @return 0 on success, -1 on error (logged to stderr)
 */
int config_load(const char *path, config_t *out);

/**
 * Save configuration as valid JSON to a file.
 *
 * Preserves light ordering.
 *
 * @param path  Path to write JSON config file
 * @param cfg   Configuration to save
 * @return 0 on success, -1 on error
 */
int config_save(const char *path, const config_t *cfg);

/**
 * Set the config file path used by config_reload.
 *
 * Must be called before config_reload. Typically called once
 * after the initial config_load succeeds.
 *
 * @param path  Path to the config file
 */
void config_set_path(const char *path);

/**
 * Re-read the config file and signal Light_UI to rebuild the tile grid.
 *
 * Uses the path previously set via config_set_path.
 * On success, destroys the current UI and re-initialises with the
 * new light list. On failure, the current UI is left unchanged.
 *
 * @return 0 on success, -1 on error
 */
int config_reload(void);

/**
 * Get a pointer to the current loaded configuration.
 *
 * Returns NULL if no config has been loaded via config_reload.
 *
 * @return Pointer to current config, or NULL
 */
const config_t *config_get_current(void);

#endif /* CONFIG_H */
