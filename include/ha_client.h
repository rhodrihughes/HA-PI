/**
 * ha_client.h — Home Assistant REST API client using libcurl
 *
 * Communicates with Home Assistant to fetch light states and toggle lights.
 * Maintains a single reusable CURL handle for connection reuse.
 * All HTTP calls are synchronous (blocking).
 *
 * Error handling:
 *   - libcurl connection errors: logged to stderr, last known states retained
 *   - HTTP 4xx/5xx: affected entity treated as UNKNOWN state
 *   - Toggle failure: optimistic state reverts on next poll cycle
 *   - Automatic retry on next poll interval (no user intervention)
 *
 * Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 5.3, 11.1, 11.2, 11.3, 11.4
 */

#ifndef HA_CLIENT_H
#define HA_CLIENT_H

#include "light_ui.h"

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

/** Home Assistant connection configuration. */
typedef struct {
    char base_url[128];   /* e.g. "http://192.168.1.100:8123" */
    char token[512];      /* Long-lived access token           */
} ha_config_t;

/**
 * Initialise the HA client with base URL and long-lived access token.
 *
 * Creates a reusable CURL handle and sets the Authorization header.
 *
 * @param base_url  HA base URL, e.g. "http://192.168.1.100:8123"
 * @param token     Long-lived access token
 * @return 0 on success, -1 on failure
 */
int ha_client_init(const char *base_url, const char *token);

/**
 * Fetch the current state of a single entity from Home Assistant.
 *
 * Sends GET /api/states/<entity_id> and parses the "state" field.
 * On connection error: logs to stderr, returns LIGHT_STATE_UNKNOWN.
 * On HTTP 4xx/5xx: returns LIGHT_STATE_UNKNOWN.
 *
 * @param entity_id  HA entity ID, e.g. "light.kitchen"
 * @return LIGHT_STATE_ON, LIGHT_STATE_OFF, or LIGHT_STATE_UNKNOWN
 */
light_state_t ha_get_state(const char *entity_id);

/**
 * Toggle a light by sending the opposite service call.
 *
 * Fetches current state, then POSTs to turn_on or turn_off.
 * On failure: returns -1; optimistic state reverts on next poll.
 *
 * @param entity_id  HA entity ID
 * @return 0 on success, -1 on failure
 */
int ha_toggle_light(const char *entity_id);

/**
 * Poll all configured lights and update the UI.
 *
 * For each light, fetches state via ha_get_state and calls
 * light_ui_set_state. On connection error, retains last known
 * tile states (skips ui update for that entity).
 *
 * @param lights  Array of light configurations
 * @param count   Number of lights
 */
void ha_poll_all(const light_config_t *lights, int count);

/**
 * Free the CURL handle and associated resources.
 */
void ha_client_cleanup(void);

#endif /* HA_CLIENT_H */
