/**
 * config_server.h — Password-protected web configuration server
 *
 * Runs a mongoose HTTP server in a background pthread, serving a web UI
 * for editing the light configuration without SSH or file editing.
 *
 * Routes:
 *   GET  /            — Login page (unauthenticated)
 *   POST /login       — Verify password, set session cookie
 *   GET  /settings    — Settings page (authenticated)
 *   GET  /api/config  — Current config as JSON (authenticated)
 *   POST /api/config  — Update config, trigger live reload (authenticated)
 *   POST /logout      — Clear session (authenticated)
 *
 * Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 10.1, 10.2, 10.3, 10.4
 */

#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#include "config.h"

/**
 * Start the config server on the given port in a background thread.
 *
 * The server runs independently of the LVGL main loop. It uses the
 * provided config pointer for reading current settings and calls
 * config_save / config_reload when settings are updated via the web UI.
 *
 * @param port  TCP port to listen on (e.g. 8080)
 * @param cfg   Pointer to the current application config
 * @return 0 on success, -1 on failure
 */
int config_server_start(int port, config_t *cfg);

/**
 * Stop the config server and join the background thread.
 *
 * Signals the mongoose event loop to exit, then joins the server
 * thread. Safe to call even if the server was never started.
 */
void config_server_stop(void);

/**
 * Set the config file path used by the server for saving.
 *
 * Must be called before config_server_start. Typically called
 * right after config_set_path in main.
 *
 * @param path  Path to the config file
 */
void config_server_set_path(const char *path);

#endif /* CONFIG_SERVER_H */
