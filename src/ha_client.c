/**
 * ha_client.c — Home Assistant REST API client using libcurl
 *
 * Implements state fetching, light toggling, and polling via the HA REST API.
 * Uses a single reusable CURL handle for connection reuse.
 *
 * Error handling (Req 11.1–11.4):
 *   - Connection errors: logged to stderr, last known states retained
 *   - HTTP 4xx/5xx: entity treated as UNKNOWN
 *   - Toggle failure: optimistic state reverts on next poll cycle
 *   - Automatic retry on next poll interval
 *
 * Requirements: 6.1–6.6, 5.3, 11.1–11.4
 */

#include "ha_client.h"
#include "light_ui.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal state                                                    */
/* ------------------------------------------------------------------ */

/** Maximum size for HTTP response buffer. */
#define HA_RESPONSE_BUF_SIZE  4096

/** Maximum URL length (base_url + path + entity_id). */
#define HA_URL_BUF_SIZE       512

/** Reusable CURL handle — created once, reused across all requests. */
static CURL *s_curl = NULL;

/** Authorization header list (set once, reused). */
static struct curl_slist *s_headers = NULL;

/** Stored base URL. */
static char s_base_url[256] = {0};

/** Buffer for accumulating HTTP response body. */
typedef struct {
    char   data[HA_RESPONSE_BUF_SIZE];
    size_t len;
} response_buf_t;

/* ------------------------------------------------------------------ */
/*  libcurl write callback                                            */
/* ------------------------------------------------------------------ */

/**
 * libcurl write callback — appends received data to response_buf_t.
 */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    response_buf_t *buf = (response_buf_t *)userdata;
    size_t bytes = size * nmemb;
    size_t space = HA_RESPONSE_BUF_SIZE - buf->len - 1; /* reserve NUL */

    if (bytes > space)
        bytes = space;

    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';

    return size * nmemb; /* return original size to avoid curl error */
}

/* ------------------------------------------------------------------ */
/*  JSON parsing helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * Extract the value of the "state" field from a JSON string.
 *
 * Simple string search — no JSON library needed. Looks for
 * "state" : "value" and copies value into out_buf.
 *
 * @param json     JSON response body
 * @param out_buf  Buffer to receive the state value
 * @param buf_size Size of out_buf
 * @return 0 on success, -1 if "state" field not found
 */
static int parse_state_field(const char *json, char *out_buf, size_t buf_size)
{
    /* Find "state" key — look for "state" followed by optional whitespace
     * and colon, then a quoted string value. */
    const char *key = json;

    while ((key = strstr(key, "\"state\"")) != NULL) {
        const char *p = key + 7; /* skip past "state" */

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        /* Expect colon */
        if (*p != ':') {
            key = p;
            continue;
        }
        p++;

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        /* Expect opening quote */
        if (*p != '"') {
            key = p;
            continue;
        }
        p++; /* skip opening quote */

        /* Copy value until closing quote */
        size_t i = 0;
        while (*p && *p != '"' && i < buf_size - 1) {
            out_buf[i++] = *p++;
        }
        out_buf[i] = '\0';
        return 0;
    }

    return -1;
}

/**
 * Map a state string to light_state_t.
 */
static light_state_t state_str_to_enum(const char *state_str)
{
    if (strcmp(state_str, "on") == 0)
        return LIGHT_STATE_ON;
    if (strcmp(state_str, "off") == 0)
        return LIGHT_STATE_OFF;
    return LIGHT_STATE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/*  Internal HTTP helpers                                             */
/* ------------------------------------------------------------------ */

/**
 * Perform a GET request and store the response body.
 *
 * @param url  Full URL to GET
 * @param resp Response buffer (cleared before use)
 * @param http_code  Output: HTTP status code (0 on connection error)
 * @return 0 on success (HTTP request completed), -1 on connection error
 */
static int ha_http_get(const char *url, response_buf_t *resp, long *http_code)
{
    CURLcode res;

    resp->len = 0;
    resp->data[0] = '\0';
    *http_code = 0;

    curl_easy_setopt(s_curl, CURLOPT_URL, url);
    curl_easy_setopt(s_curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(s_curl, CURLOPT_POST, 0L);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(s_curl, CURLOPT_WRITEDATA, resp);

    res = curl_easy_perform(s_curl);
    if (res != CURLE_OK) {
        /* Req 11.1: log connection error to stderr */
        fprintf(stderr, "ha_client: GET %s failed: %s\n",
                url, curl_easy_strerror(res));
        return -1;
    }

    curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, http_code);
    return 0;
}

/**
 * Perform a POST request with a JSON body.
 *
 * @param url       Full URL to POST
 * @param json_body JSON request body
 * @param resp      Response buffer (cleared before use)
 * @param http_code Output: HTTP status code (0 on connection error)
 * @return 0 on success (HTTP request completed), -1 on connection error
 */
static int ha_http_post(const char *url, const char *json_body,
                        response_buf_t *resp, long *http_code)
{
    CURLcode res;

    resp->len = 0;
    resp->data[0] = '\0';
    *http_code = 0;

    curl_easy_setopt(s_curl, CURLOPT_URL, url);
    curl_easy_setopt(s_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(s_curl, CURLOPT_WRITEDATA, resp);

    res = curl_easy_perform(s_curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "ha_client: POST %s failed: %s\n",
                url, curl_easy_strerror(res));
        return -1;
    }

    curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, http_code);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int ha_client_init(const char *base_url, const char *token)
{
    char auth_header[600];

    if (!base_url || !token) {
        fprintf(stderr, "ha_client: base_url and token must not be NULL\n");
        return -1;
    }

    /* Store base URL (strip trailing slash if present) */
    snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
    size_t len = strlen(s_base_url);
    if (len > 0 && s_base_url[len - 1] == '/')
        s_base_url[len - 1] = '\0';

    /* Create reusable CURL handle (Req 6.6) */
    s_curl = curl_easy_init();
    if (!s_curl) {
        fprintf(stderr, "ha_client: curl_easy_init() failed\n");
        return -1;
    }

    /* Build Authorization header (Req 6.5) */
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

    /* Set headers: Authorization + Content-Type for POST requests */
    s_headers = curl_slist_append(NULL, auth_header);
    s_headers = curl_slist_append(s_headers, "Content-Type: application/json");

    curl_easy_setopt(s_curl, CURLOPT_HTTPHEADER, s_headers);

    /* Connection timeout: 5 seconds to avoid blocking the UI too long */
    curl_easy_setopt(s_curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(s_curl, CURLOPT_TIMEOUT, 10L);

    return 0;
}

light_state_t ha_get_state(const char *entity_id)
{
    char url[HA_URL_BUF_SIZE];
    response_buf_t resp;
    long http_code = 0;
    char state_str[32] = {0};

    if (!s_curl || !entity_id)
        return LIGHT_STATE_UNKNOWN;

    /* Build URL: GET /api/states/<entity_id> (Req 6.2) */
    snprintf(url, sizeof(url), "%s/api/states/%s", s_base_url, entity_id);

    /* Perform GET request */
    if (ha_http_get(url, &resp, &http_code) != 0) {
        /* Req 11.1: connection error — return UNKNOWN, caller retains
         * last known state by not calling light_ui_set_state */
        return LIGHT_STATE_UNKNOWN;
    }

    /* Req 11.2: HTTP 4xx/5xx → treat as UNKNOWN */
    if (http_code >= 400) {
        fprintf(stderr, "ha_client: GET %s returned HTTP %ld\n",
                url, http_code);
        return LIGHT_STATE_UNKNOWN;
    }

    /* Req 6.3: parse JSON "state" field */
    if (parse_state_field(resp.data, state_str, sizeof(state_str)) != 0) {
        fprintf(stderr, "ha_client: no \"state\" field in response for %s\n",
                entity_id);
        return LIGHT_STATE_UNKNOWN;
    }

    return state_str_to_enum(state_str);
}

int ha_toggle_light(const char *entity_id)
{
    char url[HA_URL_BUF_SIZE];
    char body[128];
    response_buf_t resp;
    long http_code = 0;
    light_state_t current;

    if (!s_curl || !entity_id)
        return -1;

    /* Fetch current state to decide which service to call (Req 5.3) */
    current = ha_get_state(entity_id);

    /* Determine service endpoint:
     *   ON  → turn_off
     *   OFF → turn_on
     *   UNKNOWN → default to turn_on */
    const char *service;
    if (current == LIGHT_STATE_ON)
        service = "turn_off";
    else
        service = "turn_on";

    /* Build URL: POST /api/services/<domain>/<service>
     * Extract domain from entity_id (e.g. "light" from "light.living_room",
     * "switch" from "switch.studio_lamp") */
    char domain[64];
    const char *dot = strchr(entity_id, '.');
    if (dot) {
        size_t dlen = (size_t)(dot - entity_id);
        if (dlen >= sizeof(domain)) dlen = sizeof(domain) - 1;
        memcpy(domain, entity_id, dlen);
        domain[dlen] = '\0';
    } else {
        snprintf(domain, sizeof(domain), "light"); /* fallback */
    }

    snprintf(url, sizeof(url), "%s/api/services/%s/%s",
             s_base_url, domain, service);

    /* Build JSON body */
    snprintf(body, sizeof(body), "{\"entity_id\": \"%s\"}", entity_id);

    /* Perform POST request */
    if (ha_http_post(url, body, &resp, &http_code) != 0) {
        /* Req 11.3: toggle failure — optimistic state reverts on next poll */
        fprintf(stderr, "ha_client: toggle failed for %s (connection error)\n",
                entity_id);
        return -1;
    }

    if (http_code >= 400) {
        fprintf(stderr, "ha_client: toggle failed for %s (HTTP %ld)\n",
                entity_id, http_code);
        return -1;
    }

    return 0;
}

void ha_poll_all(const light_config_t *lights, int count)
{
    if (!s_curl || !lights || count <= 0)
        return;

    for (int i = 0; i < count; i++) {
        char url[HA_URL_BUF_SIZE];
        response_buf_t resp;
        long http_code = 0;
        char state_str[32] = {0};

        /* Build URL for this entity */
        snprintf(url, sizeof(url), "%s/api/states/%s",
                 s_base_url, lights[i].entity_id);

        /* Perform GET request */
        if (ha_http_get(url, &resp, &http_code) != 0) {
            /* Req 11.1: connection error — retain last known state.
             * Skip light_ui_set_state so tile keeps its current appearance.
             * Req 11.4: automatic retry on next poll interval. */
            continue;
        }

        /* Req 11.2: HTTP 4xx/5xx → set entity to UNKNOWN */
        if (http_code >= 400) {
            fprintf(stderr, "ha_client: poll %s returned HTTP %ld\n",
                    lights[i].entity_id, http_code);
            light_ui_set_state(i, LIGHT_STATE_UNKNOWN);
            continue;
        }

        /* Parse state and update UI (Req 6.3, 6.4) */
        if (parse_state_field(resp.data, state_str, sizeof(state_str)) != 0) {
            fprintf(stderr, "ha_client: no \"state\" field for %s\n",
                    lights[i].entity_id);
            light_ui_set_state(i, LIGHT_STATE_UNKNOWN);
            continue;
        }

        light_state_t state = state_str_to_enum(state_str);
        light_ui_set_state(i, state);
    }
}

void ha_client_cleanup(void)
{
    if (s_headers) {
        curl_slist_free_all(s_headers);
        s_headers = NULL;
    }

    if (s_curl) {
        curl_easy_cleanup(s_curl);
        s_curl = NULL;
    }

    s_base_url[0] = '\0';
}
