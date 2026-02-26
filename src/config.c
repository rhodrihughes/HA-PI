/**
 * config.c — Configuration management for HA Light Control
 *
 * Loads, validates, saves, and hot-reloads a JSON configuration file.
 * Uses manual JSON parsing (no external JSON library) consistent with
 * the rest of the codebase.
 *
 * Config file format (JSON):
 * {
 *   "ha_url": "http://192.168.1.100:8123",
 *   "ha_token": "eyJ...",
 *   "web_password_hash": "$2b$10$...",
 *   "lights": [
 *     { "entity_id": "light.living_room", "label": "Living Room", "icon": "bulb" }
 *   ]
 * }
 *
 * Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Internal state                                                    */
/* ------------------------------------------------------------------ */

/** Path used by config_reload. */
static char s_config_path[CONFIG_PATH_MAX] = {0};

/** Current loaded config (used by config_reload and config_get_current). */
static config_t s_current_config;
static int       s_config_loaded = 0;

/* ------------------------------------------------------------------ */
/*  JSON parsing helpers                                              */
/* ------------------------------------------------------------------ */

/** Skip whitespace, return pointer to next non-whitespace char. */
static const char *skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/**
 * Extract a JSON string value for a given key from a JSON object.
 *
 * Searches for "key" : "value" and copies value into out_buf.
 * Handles escaped quotes within values.
 *
 * @param json     JSON string to search
 * @param key      Key name (without quotes)
 * @param out_buf  Buffer to receive the value
 * @param buf_size Size of out_buf
 * @return 0 on success, -1 if key not found
 */
static int json_get_string(const char *json, const char *key,
                           char *out_buf, size_t buf_size)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos)
        return -1;

    pos += strlen(search);
    pos = skip_ws(pos);

    if (*pos != ':')
        return -1;
    pos++;
    pos = skip_ws(pos);

    if (*pos != '"')
        return -1;
    pos++; /* skip opening quote */

    size_t i = 0;
    while (*pos && *pos != '"' && i < buf_size - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++; /* skip backslash, take next char */
        }
        out_buf[i++] = *pos++;
    }
    out_buf[i] = '\0';
    return 0;
}

/**
 * Find the start of the "lights" JSON array.
 *
 * @param json  Full JSON string
 * @return Pointer to the '[' character, or NULL if not found
 */
static const char *find_lights_array(const char *json)
{
    const char *pos = strstr(json, "\"lights\"");
    if (!pos)
        return NULL;

    pos += 8; /* skip "lights" */
    pos = skip_ws(pos);

    if (*pos != ':')
        return NULL;
    pos++;
    pos = skip_ws(pos);

    if (*pos != '[')
        return NULL;

    return pos;
}

/**
 * Find the next JSON object '{...}' within an array.
 *
 * @param p      Pointer into the array (after '[' or after previous object)
 * @param start  Output: pointer to the '{' of the object
 * @param end    Output: pointer to the '}' of the object
 * @return 0 on success, -1 if no more objects
 */
static int find_next_object(const char *p, const char **start, const char **end)
{
    p = skip_ws(p);

    /* Skip comma between objects */
    if (*p == ',')
        p = skip_ws(p + 1);

    if (*p != '{')
        return -1;

    *start = p;

    /* Find matching closing brace (no nested objects expected) */
    int depth = 0;
    int in_string = 0;
    while (*p) {
        if (*p == '"' && (p == *start || *(p - 1) != '\\'))
            in_string = !in_string;
        if (!in_string) {
            if (*p == '{') depth++;
            if (*p == '}') {
                depth--;
                if (depth == 0) {
                    *end = p;
                    return 0;
                }
            }
        }
        p++;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Validation helpers                                                */
/* ------------------------------------------------------------------ */

/**
 * Validate that an entity_id is non-empty and matches <domain>.<name>.
 *
 * Both domain and name must be non-empty and contain only alphanumeric
 * characters and underscores.
 *
 * @param entity_id  Entity ID string to validate
 * @return 0 if valid, -1 if invalid
 */
static int validate_entity_id(const char *entity_id)
{
    if (!entity_id || entity_id[0] == '\0')
        return -1;

    /* Find the dot separator */
    const char *dot = strchr(entity_id, '.');
    if (!dot || dot == entity_id || *(dot + 1) == '\0')
        return -1;

    /* Validate domain part (before dot) */
    for (const char *p = entity_id; p < dot; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_')
            return -1;
    }

    /* Validate name part (after dot) */
    for (const char *p = dot + 1; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_')
            return -1;
    }

    return 0;
}

/**
 * Validate that a label is non-empty and ≤ 31 characters.
 *
 * @param label  Label string to validate
 * @return 0 if valid, -1 if invalid
 */
static int validate_label(const char *label)
{
    if (!label || label[0] == '\0')
        return -1;
    if (strlen(label) > 31)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  File I/O helpers                                                  */
/* ------------------------------------------------------------------ */

/**
 * Read entire file into a malloc'd buffer.
 *
 * @param path  File path
 * @param size  Output: file size in bytes
 * @return Allocated buffer (caller must free), or NULL on error
 */
static char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config: cannot open '%s': ", path);
        perror("");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fprintf(stderr, "config: empty or unreadable file '%s'\n", path);
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fprintf(stderr, "config: out of memory reading '%s'\n", path);
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    buf[read_bytes] = '\0';
    fclose(f);

    if (size)
        *size = read_bytes;

    return buf;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int config_load(const char *path, config_t *out)
{
    if (!path || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    size_t file_size = 0;
    char *json = read_file(path, &file_size);
    if (!json)
        return -1;

    /* Parse top-level string fields — ha_url and ha_token are optional
     * so the app can start without credentials and show the config UI. */
    if (json_get_string(json, "ha_url", out->ha.base_url,
                        sizeof(out->ha.base_url)) != 0) {
        fprintf(stderr, "config: 'ha_url' not set — configure via web UI\n");
        out->ha.base_url[0] = '\0';
    }

    if (json_get_string(json, "ha_token", out->ha.token,
                        sizeof(out->ha.token)) != 0) {
        fprintf(stderr, "config: 'ha_token' not set — configure via web UI\n");
        out->ha.token[0] = '\0';
    }

    /* web_password_hash is optional (may not be set yet) */
    json_get_string(json, "web_password_hash", out->web_password_hash,
                    sizeof(out->web_password_hash));

    /* Parse lights array (optional — empty config still starts the UI) */
    const char *arr = find_lights_array(json);
    if (!arr) {
        fprintf(stderr, "config: no 'lights' array — starting with 0 lights\n");
        out->light_count = 0;
        free(json);
        return 0;
    }

    const char *p = arr + 1; /* skip '[' */
    int count = 0;

    while (count < CONFIG_MAX_LIGHTS) {
        const char *obj_start = NULL;
        const char *obj_end = NULL;

        if (find_next_object(p, &obj_start, &obj_end) != 0)
            break;

        /* Extract a temporary copy of this object for parsing */
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        char *obj_buf = malloc(obj_len + 1);
        if (!obj_buf) {
            fprintf(stderr, "config: out of memory parsing light %d\n", count);
            free(json);
            return -1;
        }
        memcpy(obj_buf, obj_start, obj_len);
        obj_buf[obj_len] = '\0';

        light_config_t *light = &out->lights[count];

        if (json_get_string(obj_buf, "entity_id", light->entity_id,
                            sizeof(light->entity_id)) != 0) {
            fprintf(stderr, "config: light %d missing 'entity_id'\n", count);
            free(obj_buf);
            free(json);
            return -1;
        }

        if (json_get_string(obj_buf, "label", light->label,
                            sizeof(light->label)) != 0) {
            fprintf(stderr, "config: light %d missing 'label'\n", count);
            free(obj_buf);
            free(json);
            return -1;
        }

        if (json_get_string(obj_buf, "icon", light->icon,
                            sizeof(light->icon)) != 0) {
            fprintf(stderr, "config: light %d missing 'icon'\n", count);
            free(obj_buf);
            free(json);
            return -1;
        }

        free(obj_buf);

        /* Validate this light entry */
        if (validate_entity_id(light->entity_id) != 0) {
            fprintf(stderr, "config: light %d invalid entity_id '%s' "
                    "(must be <domain>.<name>)\n", count, light->entity_id);
            free(json);
            return -1;
        }

        if (validate_label(light->label) != 0) {
            fprintf(stderr, "config: light %d invalid label '%s' "
                    "(must be non-empty, max 31 chars)\n", count, light->label);
            free(json);
            return -1;
        }

        count++;
        p = obj_end + 1;
    }

    /* Check if there are more lights beyond the limit */
    {
        const char *check = p;
        const char *extra_start = NULL;
        const char *extra_end = NULL;
        if (count >= CONFIG_MAX_LIGHTS &&
            find_next_object(check, &extra_start, &extra_end) == 0) {
            fprintf(stderr, "config: too many lights (max %d)\n",
                    CONFIG_MAX_LIGHTS);
            free(json);
            return -1;
        }
    }

    out->light_count = count;
    free(json);
    return 0;
}

int config_save(const char *path, const config_t *cfg)
{
    if (!path || !cfg)
        return -1;

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "config: cannot open '%s' for writing: ", path);
        perror("");
        return -1;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"ha_url\": \"%s\",\n", cfg->ha.base_url);
    fprintf(f, "  \"ha_token\": \"%s\",\n", cfg->ha.token);
    fprintf(f, "  \"web_password_hash\": \"%s\",\n", cfg->web_password_hash);
    fprintf(f, "  \"lights\": [\n");

    for (int i = 0; i < cfg->light_count; i++) {
        const light_config_t *l = &cfg->lights[i];
        fprintf(f, "    { \"entity_id\": \"%s\", \"label\": \"%s\", \"icon\": \"%s\" }",
                l->entity_id, l->label, l->icon);
        if (i < cfg->light_count - 1)
            fprintf(f, ",");
        fprintf(f, "\n");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

void config_set_path(const char *path)
{
    if (path)
        snprintf(s_config_path, sizeof(s_config_path), "%s", path);
}

int config_reload(void)
{
    if (s_config_path[0] == '\0') {
        fprintf(stderr, "config: reload path not set (call config_set_path first)\n");
        return -1;
    }

    config_t new_cfg;
    if (config_load(s_config_path, &new_cfg) != 0) {
        fprintf(stderr, "config: reload failed, keeping current config\n");
        return -1;
    }

    /* Destroy current UI and rebuild with new config */
    light_ui_destroy();
    light_ui_init(new_cfg.lights, new_cfg.light_count);

    /* Update stored config */
    s_current_config = new_cfg;
    s_config_loaded = 1;

    return 0;
}

const config_t *config_get_current(void)
{
    return s_config_loaded ? &s_current_config : NULL;
}
