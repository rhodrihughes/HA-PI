/**
 * config_server.c — Password-protected web configuration server
 *
 * Mongoose HTTP server running in a background pthread. Serves a login
 * page, settings page, and JSON API for editing the light configuration.
 *
 * Session management:
 *   - Tokens are random 32-byte hex strings (64 hex chars)
 *   - Sessions expire after 1 hour of inactivity
 *   - Unauthenticated requests to protected routes redirect to login
 *   - Web password stored as bcrypt hash in config file
 *
 * Requirements: 9.1–9.7, 10.1–10.4
 */

#include "config_server.h"
#include "mongoose.h"

#include <crypt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define MAX_SESSIONS        8
#define SESSION_TOKEN_BYTES 32
#define SESSION_TOKEN_HEX   (SESSION_TOKEN_BYTES * 2)  /* 64 hex chars */
#define SESSION_TIMEOUT_SEC 3600  /* 1 hour */
#define LOGIN_FAIL_DELAY_MS 1000 /* 1 second delay on wrong password */

/* ------------------------------------------------------------------ */
/*  Session storage                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char   token[SESSION_TOKEN_HEX + 1]; /* hex string + NUL */
    time_t last_active;                  /* 0 = slot unused   */
} session_t;

static session_t s_sessions[MAX_SESSIONS];
static pthread_mutex_t s_session_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Server state                                                      */
/* ------------------------------------------------------------------ */

static struct mg_mgr   s_mgr;
static pthread_t       s_thread;
static volatile int    s_running;
static config_t       *s_cfg;
static char            s_config_file_path[CONFIG_PATH_MAX];

/* ------------------------------------------------------------------ */
/*  Session helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * Generate a random 32-byte hex token using /dev/urandom.
 * Writes exactly 64 hex chars + NUL into buf (must be >= 65 bytes).
 * Returns 0 on success, -1 on failure.
 */
static int generate_session_token(char *buf, size_t buf_size)
{
    unsigned char raw[SESSION_TOKEN_BYTES];
    FILE *f;
    size_t i;

    if (buf_size < SESSION_TOKEN_HEX + 1)
        return -1;

    f = fopen("/dev/urandom", "rb");
    if (!f) {
        fprintf(stderr, "config_server: cannot open /dev/urandom\n");
        return -1;
    }
    if (fread(raw, 1, SESSION_TOKEN_BYTES, f) != SESSION_TOKEN_BYTES) {
        fclose(f);
        fprintf(stderr, "config_server: short read from /dev/urandom\n");
        return -1;
    }
    fclose(f);

    for (i = 0; i < SESSION_TOKEN_BYTES; i++)
        snprintf(buf + i * 2, 3, "%02x", raw[i]);
    buf[SESSION_TOKEN_HEX] = '\0';

    return 0;
}

/**
 * Create a new session and return its token.
 * Evicts the oldest session if all slots are full.
 * Returns 0 on success, -1 on failure.
 */
static int session_create(char *token_out, size_t token_size)
{
    int i, oldest_idx = 0;
    time_t oldest_time;

    if (generate_session_token(token_out, token_size) != 0)
        return -1;

    pthread_mutex_lock(&s_session_lock);

    /* Find a free slot or the oldest session to evict */
    oldest_time = s_sessions[0].last_active;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].last_active == 0) {
            oldest_idx = i;
            break;
        }
        if (s_sessions[i].last_active < oldest_time) {
            oldest_time = s_sessions[i].last_active;
            oldest_idx = i;
        }
    }

    snprintf(s_sessions[oldest_idx].token,
             sizeof(s_sessions[oldest_idx].token), "%s", token_out);
    s_sessions[oldest_idx].last_active = time(NULL);

    pthread_mutex_unlock(&s_session_lock);
    return 0;
}

/**
 * Validate a session token. Returns 1 if valid, 0 if invalid/expired.
 * Updates last_active on valid sessions.
 */
static int session_validate(const char *token)
{
    int i;
    time_t now;

    if (!token || token[0] == '\0')
        return 0;

    now = time(NULL);
    pthread_mutex_lock(&s_session_lock);

    for (i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].last_active == 0)
            continue;

        /* Check expiry first */
        if (now - s_sessions[i].last_active > SESSION_TIMEOUT_SEC) {
            s_sessions[i].last_active = 0;  /* expire it */
            s_sessions[i].token[0] = '\0';
            continue;
        }

        if (strcmp(s_sessions[i].token, token) == 0) {
            s_sessions[i].last_active = now;  /* refresh */
            pthread_mutex_unlock(&s_session_lock);
            return 1;
        }
    }

    pthread_mutex_unlock(&s_session_lock);
    return 0;
}

/**
 * Destroy a session by token.
 */
static void session_destroy(const char *token)
{
    int i;

    if (!token || token[0] == '\0')
        return;

    pthread_mutex_lock(&s_session_lock);
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].last_active != 0 &&
            strcmp(s_sessions[i].token, token) == 0) {
            s_sessions[i].last_active = 0;
            s_sessions[i].token[0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&s_session_lock);
}

/* ------------------------------------------------------------------ */
/*  Cookie / auth helpers                                             */
/* ------------------------------------------------------------------ */

/**
 * Extract the session token from the "session" cookie in the request.
 * Returns the token string in buf, or empty string if not found.
 */
static void get_session_cookie(struct mg_http_message *hm, char *buf, size_t buf_size)
{
    struct mg_str *cookie_hdr = mg_http_get_header(hm, "Cookie");
    struct mg_str val;

    buf[0] = '\0';
    if (!cookie_hdr)
        return;

    val = mg_http_get_header_var(*cookie_hdr, mg_str("session"));
    if (val.len > 0 && val.len < buf_size) {
        memcpy(buf, val.buf, val.len);
        buf[val.len] = '\0';
    }
}

/**
 * Check if the current request is authenticated.
 * Returns 1 if valid session, 0 otherwise.
 */
static int is_authenticated(struct mg_http_message *hm)
{
    char token[SESSION_TOKEN_HEX + 1];
    get_session_cookie(hm, token, sizeof(token));
    return session_validate(token);
}

/**
 * Verify a plaintext password against the stored bcrypt hash.
 * Returns 1 on match, 0 on mismatch or error.
 */
static int verify_password(const char *password, const char *hash)
{
    char *result;
    struct crypt_data data;

    if (!password || !hash || hash[0] == '\0')
        return 0;

    memset(&data, 0, sizeof(data));
    result = crypt_r(password, hash, &data);
    if (!result)
        return 0;

    return strcmp(result, hash) == 0;
}

/**
 * URL-decode a string in-place. Handles %XX and '+' (space).
 */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    size_t si = 0;
    size_t src_len = strlen(src);

    while (si < src_len && di < dst_size - 1) {
        if (src[si] == '%' && si + 2 < src_len) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 3;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

/**
 * Extract a form field value from URL-encoded POST body.
 * Returns length of extracted value, or 0 if not found.
 */
static size_t get_form_var(struct mg_http_message *hm, const char *name,
                           char *buf, size_t buf_size)
{
    char encoded[512];
    int len = mg_http_get_var(&hm->body, name, encoded, sizeof(encoded));
    if (len <= 0) {
        buf[0] = '\0';
        return 0;
    }
    url_decode(buf, encoded, buf_size);
    return strlen(buf);
}

/* ------------------------------------------------------------------ */
/*  HTML page helpers                                                 */
/* ------------------------------------------------------------------ */

static void serve_login_page(struct mg_connection *c, const char *error_msg)
{
    mg_http_reply(c, 200, "Content-Type: text/html\r\n",
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>HA Lights Controller</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#eee;"
        "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
        ".card{background:#16213e;padding:40px;border-radius:12px;text-align:center;"
        "max-width:360px;width:100%%}"
        "h1{margin:0 0 8px;font-size:1.5em}"
        "p.desc{color:#aaa;margin:0 0 24px;font-size:0.9em}"
        "input[type=password]{width:100%%;padding:12px;border:1px solid #333;"
        "border-radius:6px;background:#0f3460;color:#eee;font-size:1em;"
        "box-sizing:border-box;margin-bottom:16px}"
        "button{width:100%%;padding:12px;border:none;border-radius:6px;"
        "background:#e94560;color:#fff;font-size:1em;cursor:pointer}"
        "button:hover{background:#c73e54}"
        ".error{color:#e94560;margin:0 0 16px;font-size:0.9em}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>&#128161; HA Lights Controller</h1>"
        "<p class='desc'>Manage the light buttons shown on your Raspberry Pi display.</p>"
        "%s"
        "<form method='POST' action='/login'>"
        "<input type='password' name='password' placeholder='Password' autofocus>"
        "<button type='submit'>Unlock Settings</button>"
        "</form></div></body></html>",
        error_msg ? error_msg : "");
}

static void serve_settings_page(struct mg_connection *c)
{
    mg_http_reply(c, 200, "Content-Type: text/html\r\n",
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Settings — HA Lights</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px}"
        ".container{max-width:600px;margin:0 auto}"
        "h1{font-size:1.4em;margin-bottom:4px}"
        ".topbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}"
        "a.logout{color:#e94560;text-decoration:none;font-size:0.9em}"
        "label{display:block;color:#aaa;font-size:0.85em;margin-bottom:4px;margin-top:12px}"
        "input[type=text],input[type=url]{width:100%%;padding:10px;border:1px solid #333;"
        "border-radius:6px;background:#0f3460;color:#eee;font-size:0.95em;box-sizing:border-box}"
        ".light-row{display:flex;gap:8px;align-items:center;margin-bottom:8px}"
        ".light-row input{flex:1;padding:8px;border:1px solid #333;border-radius:6px;"
        "background:#0f3460;color:#eee;font-size:0.9em}"
        ".light-row .icon-field{max-width:60px}"
        ".btn{padding:10px 20px;border:none;border-radius:6px;cursor:pointer;font-size:0.95em}"
        ".btn-primary{background:#e94560;color:#fff}"
        ".btn-primary:hover{background:#c73e54}"
        ".btn-secondary{background:#16213e;color:#aaa;border:1px solid #333}"
        ".btn-danger{background:transparent;color:#e94560;border:none;font-size:1.2em;"
        "cursor:pointer;padding:4px 8px}"
        ".actions{margin-top:20px;display:flex;gap:10px}"
        "#status{margin-top:12px;font-size:0.9em;color:#aaa}"
        "</style></head><body>"
        "<div class='container'>"
        "<div class='topbar'><h1>&#128161; Settings</h1>"
        "<a class='logout' href='#' onclick=\"fetch('/logout',{method:'POST'})"
        ".then(()=>location.href='/')\">Logout</a></div>"
        "<label>Home Assistant URL</label>"
        "<input type='url' id='ha_url' placeholder='http://192.168.1.100:8123'>"
        "<label>Home Assistant Token</label>"
        "<input type='text' id='ha_token' placeholder='Long-lived access token'>"
        "<label>Lights</label>"
        "<div id='lights'></div>"
        "<button class='btn btn-secondary' onclick='addLight()'>+ Add Light</button>"
        "<div class='actions'>"
        "<button class='btn btn-primary' onclick='saveConfig()'>Save &amp; Reload</button>"
        "</div>"
        "<div id='status'></div>"
        "</div>"
        "<script>"
        "let cfg={};"
        "function renderLights(){"
        "  let h='';"
        "  (cfg.lights||[]).forEach((l,i)=>{"
        "    h+='<div class=\"light-row\">'"
        "      +'<input placeholder=\"Label\" value=\"'+esc(l.label)+'\" data-i=\"'+i+'\" data-f=\"label\">'"
        "      +'<input placeholder=\"entity_id\" value=\"'+esc(l.entity_id)+'\" data-i=\"'+i+'\" data-f=\"entity_id\">'"
        "      +'<input class=\"icon-field\" placeholder=\"Icon\" value=\"'+esc(l.icon)+'\" data-i=\"'+i+'\" data-f=\"icon\">'"
        "      +'<button class=\"btn-danger\" onclick=\"removeLight('+i+')\">&#10005;</button>'"
        "      +'</div>';"
        "  });"
        "  document.getElementById('lights').innerHTML=h;"
        "}"
        "function esc(s){return (s||\"\").replace(/&/g,'&amp;').replace(/\"/g,'&quot;').replace(/</g,'&lt;')}"
        "function addLight(){cfg.lights=cfg.lights||[];cfg.lights.push({entity_id:'',label:'',icon:'bulb'});renderLights()}"
        "function removeLight(i){cfg.lights.splice(i,1);renderLights()}"
        "function gatherConfig(){"
        "  cfg.ha_url=document.getElementById('ha_url').value;"
        "  cfg.ha_token=document.getElementById('ha_token').value;"
        "  cfg.lights=[];"
        "  document.querySelectorAll('.light-row').forEach(row=>{"
        "    let l={};"
        "    row.querySelectorAll('input').forEach(inp=>{"
        "      l[inp.dataset.f]=inp.value;"
        "    });"
        "    if(l.entity_id)cfg.lights.push(l);"
        "  });"
        "  return cfg;"
        "}"
        "function saveConfig(){"
        "  let c=gatherConfig();"
        "  document.getElementById('status').textContent='Saving...';"
        "  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
        "    body:JSON.stringify(c)})"
        "  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.json()})"
        "  .then(()=>{document.getElementById('status').textContent='Saved and reloaded!';"
        "    setTimeout(()=>document.getElementById('status').textContent='',3000)})"
        "  .catch(e=>{document.getElementById('status').textContent='Error: '+e.message})"
        "}"
        "fetch('/api/config').then(r=>r.json()).then(d=>{"
        "  cfg=d;"
        "  document.getElementById('ha_url').value=d.ha_url||'';"
        "  document.getElementById('ha_token').value=d.ha_token||'';"
        "  renderLights();"
        "}).catch(()=>location.href='/');"
        "</script></body></html>");
}

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                      */
/* ------------------------------------------------------------------ */

/**
 * Escape a string for JSON output. Writes to dst (must be large enough).
 */
static void json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;
    while (*src && di < dst_size - 2) {
        if (*src == '"' || *src == '\\') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

/**
 * Send the current config as a JSON response.
 */
static void serve_config_json(struct mg_connection *c)
{
    char url_esc[256], token_esc[1024];
    char body[4096];
    int off, i;

    json_escape(url_esc, sizeof(url_esc), s_cfg->ha.base_url);
    json_escape(token_esc, sizeof(token_esc), s_cfg->ha.token);

    off = snprintf(body, sizeof(body),
        "{\"ha_url\":\"%s\",\"ha_token\":\"%s\",\"lights\":[",
        url_esc, token_esc);

    for (i = 0; i < s_cfg->light_count && (size_t)off < sizeof(body) - 200; i++) {
        char eid[128], lbl[64], ico[16];
        json_escape(eid, sizeof(eid), s_cfg->lights[i].entity_id);
        json_escape(lbl, sizeof(lbl), s_cfg->lights[i].label);
        json_escape(ico, sizeof(ico), s_cfg->lights[i].icon);
        off += snprintf(body + off, sizeof(body) - (size_t)off,
            "%s{\"entity_id\":\"%s\",\"label\":\"%s\",\"icon\":\"%s\"}",
            i > 0 ? "," : "", eid, lbl, ico);
    }

    off += snprintf(body + off, sizeof(body) - (size_t)off, "]}");

    mg_http_reply(c, 200,
        "Content-Type: application/json\r\n", "%s", body);
}

/* ------------------------------------------------------------------ */
/*  Simple JSON parser for incoming config                            */
/* ------------------------------------------------------------------ */

/**
 * Extract a JSON string value by key from a JSON object string.
 * Copies the value into buf. Returns length of value, or -1 if not found.
 */
static int json_extract_str(const char *json, int json_len,
                            const char *path, char *buf, int buf_size)
{
    struct mg_str j = mg_str_n(json, (size_t)json_len);
    char *val;
    int len;

    val = mg_json_get_str(j, path);
    if (!val)
        return -1;

    len = (int)strlen(val);
    if (len >= buf_size)
        len = buf_size - 1;
    memcpy(buf, val, (size_t)len);
    buf[len] = '\0';
    free(val);
    return len;
}

/* ------------------------------------------------------------------ */
/*  Config update from POST /api/config                               */
/* ------------------------------------------------------------------ */

static int handle_config_update(struct mg_http_message *hm)
{
    config_t new_cfg;
    const char *json = hm->body.buf;
    int json_len = (int)hm->body.len;
    char tmp[512];
    int i, count;
    char path[64];

    memset(&new_cfg, 0, sizeof(new_cfg));

    /* Copy existing password hash (not editable via web UI) */
    snprintf(new_cfg.web_password_hash, sizeof(new_cfg.web_password_hash),
             "%s", s_cfg->web_password_hash);

    /* Extract ha_url */
    if (json_extract_str(json, json_len, "$.ha_url", tmp, sizeof(tmp)) > 0)
        snprintf(new_cfg.ha.base_url, sizeof(new_cfg.ha.base_url), "%s", tmp);

    /* Extract ha_token */
    if (json_extract_str(json, json_len, "$.ha_token", tmp, sizeof(tmp)) > 0)
        snprintf(new_cfg.ha.token, sizeof(new_cfg.ha.token), "%s", tmp);

    /* Extract lights array */
    count = 0;
    for (i = 0; i < CONFIG_MAX_LIGHTS; i++) {
        snprintf(path, sizeof(path), "$.lights[%d].entity_id", i);
        if (json_extract_str(json, json_len, path, tmp, sizeof(tmp)) <= 0)
            break;
        snprintf(new_cfg.lights[count].entity_id,
                 sizeof(new_cfg.lights[count].entity_id), "%s", tmp);

        snprintf(path, sizeof(path), "$.lights[%d].label", i);
        if (json_extract_str(json, json_len, path, tmp, sizeof(tmp)) > 0)
            snprintf(new_cfg.lights[count].label,
                     sizeof(new_cfg.lights[count].label), "%s", tmp);

        snprintf(path, sizeof(path), "$.lights[%d].icon", i);
        if (json_extract_str(json, json_len, path, tmp, sizeof(tmp)) > 0)
            snprintf(new_cfg.lights[count].icon,
                     sizeof(new_cfg.lights[count].icon), "%s", tmp);

        count++;
    }
    new_cfg.light_count = count;

    /* Save to disk and trigger live reload */
    if (config_save(s_config_file_path, &new_cfg) != 0) {
        fprintf(stderr, "config_server: failed to save config\n");
        return -1;
    }

    if (config_reload() != 0) {
        fprintf(stderr, "config_server: failed to reload config\n");
        return -1;
    }

    /* Update our local pointer to reflect the reloaded config */
    {
        const config_t *reloaded = config_get_current();
        if (reloaded)
            *s_cfg = *reloaded;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main mongoose event handler                                       */
/* ------------------------------------------------------------------ */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    struct mg_http_message *hm;

    if (ev != MG_EV_HTTP_MSG)
        return;

    hm = (struct mg_http_message *)ev_data;

    /* ---- GET / — Login page ---- */
    if (mg_match(hm->uri, mg_str("/"), NULL) &&
        mg_strcmp(hm->method, mg_str("GET")) == 0) {
        /* If already authenticated, redirect to settings */
        if (is_authenticated(hm)) {
            mg_http_reply(c, 302,
                "Location: /settings\r\n", "");
        } else {
            serve_login_page(c, NULL);
        }
        return;
    }

    /* ---- POST /login — Authenticate ---- */
    if (mg_match(hm->uri, mg_str("/login"), NULL) &&
        mg_strcmp(hm->method, mg_str("POST")) == 0) {
        char password[256];
        get_form_var(hm, "password", password, sizeof(password));

        if (verify_password(password, s_cfg->web_password_hash)) {
            char token[SESSION_TOKEN_HEX + 1];
            if (session_create(token, sizeof(token)) == 0) {
                char cookie_hdr[256];
                snprintf(cookie_hdr, sizeof(cookie_hdr),
                    "Set-Cookie: session=%s; Path=/; HttpOnly; SameSite=Strict\r\n"
                    "Location: /settings\r\n",
                    token);
                mg_http_reply(c, 302, cookie_hdr, "");
            } else {
                serve_login_page(c, "<p class='error'>Server error. Try again.</p>");
            }
        } else {
            /* 1-second delay on wrong password to slow brute force */
            usleep(LOGIN_FAIL_DELAY_MS * 1000);
            serve_login_page(c, "<p class='error'>Incorrect password.</p>");
        }
        return;
    }

    /* ---- POST /logout — Clear session ---- */
    if (mg_match(hm->uri, mg_str("/logout"), NULL) &&
        mg_strcmp(hm->method, mg_str("POST")) == 0) {
        char token[SESSION_TOKEN_HEX + 1];
        get_session_cookie(hm, token, sizeof(token));
        session_destroy(token);
        mg_http_reply(c, 200,
            "Set-Cookie: session=; Path=/; Max-Age=0\r\n"
            "Content-Type: application/json\r\n",
            "{\"ok\":true}");
        return;
    }

    /* ---- All routes below require authentication ---- */
    if (!is_authenticated(hm)) {
        mg_http_reply(c, 302,
            "Location: /\r\n", "");
        return;
    }

    /* ---- GET /settings — Settings page ---- */
    if (mg_match(hm->uri, mg_str("/settings"), NULL) &&
        mg_strcmp(hm->method, mg_str("GET")) == 0) {
        serve_settings_page(c);
        return;
    }

    /* ---- GET /api/config — Return current config as JSON ---- */
    if (mg_match(hm->uri, mg_str("/api/config"), NULL) &&
        mg_strcmp(hm->method, mg_str("GET")) == 0) {
        serve_config_json(c);
        return;
    }

    /* ---- POST /api/config — Update config and reload ---- */
    if (mg_match(hm->uri, mg_str("/api/config"), NULL) &&
        mg_strcmp(hm->method, mg_str("POST")) == 0) {
        if (handle_config_update(hm) == 0) {
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n",
                "{\"ok\":true}");
        } else {
            mg_http_reply(c, 500,
                "Content-Type: application/json\r\n",
                "{\"error\":\"Failed to save config\"}");
        }
        return;
    }

    /* ---- 404 for anything else ---- */
    mg_http_reply(c, 404, "Content-Type: text/plain\r\n", "Not found");
}

/* ------------------------------------------------------------------ */
/*  Server thread                                                     */
/* ------------------------------------------------------------------ */

static void *server_thread(void *arg)
{
    (void)arg;

    while (s_running) {
        mg_mgr_poll(&s_mgr, 200);  /* 200ms poll interval */
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int config_server_start(int port, config_t *cfg)
{
    char listen_addr[32];
    struct mg_connection *nc;

    if (s_running) {
        fprintf(stderr, "config_server: already running\n");
        return -1;
    }

    s_cfg = cfg;

    mg_mgr_init(&s_mgr);

    snprintf(listen_addr, sizeof(listen_addr), "http://0.0.0.0:%d", port);
    nc = mg_http_listen(&s_mgr, listen_addr, ev_handler, NULL);
    if (!nc) {
        fprintf(stderr, "config_server: failed to listen on port %d\n", port);
        mg_mgr_free(&s_mgr);
        return -1;
    }

    /* Clear all sessions */
    memset(s_sessions, 0, sizeof(s_sessions));

    s_running = 1;

    if (pthread_create(&s_thread, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "config_server: failed to create thread\n");
        s_running = 0;
        mg_mgr_free(&s_mgr);
        return -1;
    }

    fprintf(stderr, "config_server: listening on port %d\n", port);
    return 0;
}

void config_server_stop(void)
{
    if (!s_running)
        return;

    s_running = 0;
    pthread_join(s_thread, NULL);
    mg_mgr_free(&s_mgr);

    fprintf(stderr, "config_server: stopped\n");
}

/**
 * Set the config file path used by the config server for saving.
 * This should be called before config_server_start, typically
 * right after config_set_path in main.
 */
void config_server_set_path(const char *path)
{
    if (path)
        snprintf(s_config_file_path, sizeof(s_config_file_path), "%s", path);
}
