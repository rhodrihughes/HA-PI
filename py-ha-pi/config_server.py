"""Web configuration server using Flask in a background thread."""

import logging
import os
import secrets
import threading
import time

from flask import Flask, request, redirect, session, jsonify, render_template_string

import config
import ha_client

log = logging.getLogger("config_server")

_app: Flask | None = None
_thread: threading.Thread | None = None
_cfg: dict | None = None
_reload_callback = None

SESSION_TIMEOUT = 3600  # 1 hour

# --------------------------------------------------------------------------
#  HTML templates
# --------------------------------------------------------------------------

LOGIN_HTML = """<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HA Lights Controller</title>
<style>
body{font-family:sans-serif;background:#1a1a2e;color:#eee;
display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
.card{background:#16213e;padding:40px;border-radius:12px;text-align:center;max-width:360px;width:100%}
h1{margin:0 0 8px;font-size:1.5em}
p.desc{color:#aaa;margin:0 0 24px;font-size:0.9em}
input[type=password]{width:100%;padding:12px;border:1px solid #333;
border-radius:6px;background:#0f3460;color:#eee;font-size:1em;box-sizing:border-box;margin-bottom:16px}
button{width:100%;padding:12px;border:none;border-radius:6px;
background:#e94560;color:#fff;font-size:1em;cursor:pointer}
button:hover{background:#c73e54}
.error{color:#e94560;margin:0 0 16px;font-size:0.9em}
</style></head><body>
<div class="card">
<h1>&#128161; HA Lights Controller</h1>
<p class="desc">Manage the light buttons shown on your Raspberry Pi display.</p>
{% if error %}<p class="error">{{ error }}</p>{% endif %}
<form method="POST" action="/login">
<input type="password" name="password" placeholder="Password" autofocus>
<button type="submit">Unlock Settings</button>
</form></div></body></html>"""

SETTINGS_HTML = """<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Settings — HA Lights</title>
<style>
body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px}
.container{max-width:600px;margin:0 auto}
h1{font-size:1.4em;margin-bottom:4px}
.topbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}
a.logout{color:#e94560;text-decoration:none;font-size:0.9em}
label{display:block;color:#aaa;font-size:0.85em;margin-bottom:4px;margin-top:12px}
input[type=text]{width:100%;padding:10px;border:1px solid #333;
border-radius:6px;background:#0f3460;color:#eee;font-size:0.95em;box-sizing:border-box}
.light-row{display:flex;gap:8px;align-items:center;margin-bottom:8px}
.light-row input{flex:1;padding:8px;border:1px solid #333;border-radius:6px;
background:#0f3460;color:#eee;font-size:0.9em}
.light-row .icon-field{max-width:60px}
.btn{padding:10px 20px;border:none;border-radius:6px;cursor:pointer;font-size:0.95em}
.btn-primary{background:#e94560;color:#fff}
.btn-primary:hover{background:#c73e54}
.btn-secondary{background:#16213e;color:#aaa;border:1px solid #333}
.btn-danger{background:transparent;color:#e94560;border:none;font-size:1.2em;cursor:pointer;padding:4px 8px}
.btn-test{background:#0f3460;color:#aaa;border:1px solid #333;margin-top:12px}
.btn-test:hover{background:#16213e}
#test-result{margin-top:8px;font-size:0.85em;min-height:1.2em}
.test-ok{color:#4caf50}.test-fail{color:#e94560}
.actions{margin-top:20px;display:flex;gap:10px}
#status{margin-top:12px;font-size:0.9em;color:#aaa}
</style></head><body>
<div class="container">
<div class="topbar"><h1>&#128161; Settings</h1>
<a class="logout" href="#" onclick="fetch('/logout',{method:'POST'}).then(()=>location.href='/')">Logout</a></div>
<label>Home Assistant URL</label>
<input type="text" id="ha_url" placeholder="e.g. http://192.168.1.100:8123">
<label>Home Assistant Token</label>
<input type="text" id="ha_token" placeholder="e.g. eyJhbGciOiJIUzI1NiIs...">
<button class="btn btn-test" onclick="testConnection()">Test Connection</button>
<div id="test-result"></div>
<label>Lights</label>
<div id="lights"></div>
<button class="btn btn-secondary" onclick="addLight()">+ Add Light</button>
<div class="actions">
<button class="btn btn-primary" onclick="saveConfig()">Save &amp; Reload</button>
</div>
<div id="status"></div>
</div>
<script>
let cfg={};
function esc(s){return(s||"").replace(/&/g,"&amp;").replace(/"/g,"&quot;").replace(/</g,"&lt;")}
function renderLights(){
  let h="";
  (cfg.lights||[]).forEach((l,i)=>{
    h+='<div class="light-row">'
      +'<input placeholder="e.g. Living Room" value="'+esc(l.label)+'" data-i="'+i+'" data-f="label">'
      +'<input placeholder="e.g. light.living_room" value="'+esc(l.entity_id)+'" data-i="'+i+'" data-f="entity_id">'
      +'<input class="icon-field" placeholder="bulb" value="'+esc(l.icon)+'" data-i="'+i+'" data-f="icon">'
      +'<button class="btn-danger" onclick="removeLight('+i+')">&#10005;</button></div>';
  });
  document.getElementById("lights").innerHTML=h;
}
function addLight(){cfg.lights=cfg.lights||[];cfg.lights.push({entity_id:"",label:"",icon:"bulb"});renderLights()}
function removeLight(i){cfg.lights.splice(i,1);renderLights()}
function gatherConfig(){
  cfg.ha_url=document.getElementById("ha_url").value;
  cfg.ha_token=document.getElementById("ha_token").value;
  cfg.lights=[];
  document.querySelectorAll(".light-row").forEach(row=>{
    let l={};row.querySelectorAll("input").forEach(inp=>{l[inp.dataset.f]=inp.value});
    if(l.entity_id)cfg.lights.push(l);
  });
  return cfg;
}
function testConnection(){
  var el=document.getElementById("test-result");
  el.className="";el.textContent="Testing...";
  var u=document.getElementById("ha_url").value;
  var t=document.getElementById("ha_token").value;
  fetch("/api/test-connection",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({ha_url:u,ha_token:t})})
  .then(r=>r.json())
  .then(d=>{el.className=d.ok?"test-ok":"test-fail";el.textContent=d.message})
  .catch(e=>{el.className="test-fail";el.textContent="Request failed: "+e.message})
}
function saveConfig(){
  let c=gatherConfig();
  document.getElementById("status").textContent="Saving...";
  fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify(c)})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.json()})
  .then(()=>{document.getElementById("status").textContent="Saved and reloaded!";
    setTimeout(()=>document.getElementById("status").textContent="",3000)})
  .catch(e=>{document.getElementById("status").textContent="Error: "+e.message})
}
fetch("/api/config").then(r=>r.json()).then(d=>{
  cfg=d;
  document.getElementById("ha_url").value=d.ha_url||"";
  document.getElementById("ha_token").value=d.ha_token||"";
  renderLights();
}).catch(()=>location.href="/");
</script></body></html>"""


# --------------------------------------------------------------------------
#  Flask app factory
# --------------------------------------------------------------------------

def _create_app(cfg_ref: dict, reload_cb) -> Flask:
    app = Flask(__name__)
    app.secret_key = secrets.token_hex(32)

    @app.before_request
    def check_session():
        """Expire sessions after timeout."""
        if "logged_in" in session:
            last = session.get("last_active", 0)
            if time.time() - last > SESSION_TIMEOUT:
                session.clear()
            else:
                session["last_active"] = time.time()

    @app.route("/")
    def index():
        if session.get("logged_in"):
            return redirect("/settings")
        return render_template_string(LOGIN_HTML, error=None)

    @app.route("/login", methods=["POST"])
    def login():
        password = request.form.get("password", "")
        if password and password == cfg_ref.get("web_password", ""):
            session["logged_in"] = True
            session["last_active"] = time.time()
            return redirect("/settings")
        time.sleep(1)  # brute-force delay
        return render_template_string(LOGIN_HTML, error="Incorrect password.")

    @app.route("/logout", methods=["POST"])
    def logout():
        session.clear()
        return jsonify(ok=True)

    @app.route("/settings")
    def settings():
        if not session.get("logged_in"):
            return redirect("/")
        return render_template_string(SETTINGS_HTML)

    @app.route("/api/config", methods=["GET"])
    def get_config():
        if not session.get("logged_in"):
            return redirect("/")
        return jsonify(
            ha_url=cfg_ref.get("ha_url", ""),
            ha_token=cfg_ref.get("ha_token", ""),
            lights=cfg_ref.get("lights", []),
        )

    @app.route("/api/config", methods=["POST"])
    def update_config():
        if not session.get("logged_in"):
            return jsonify(error="Unauthorized"), 401
        data = request.get_json(force=True)
        cfg_ref["ha_url"] = data.get("ha_url", "")
        cfg_ref["ha_token"] = data.get("ha_token", "")
        cfg_ref["lights"] = data.get("lights", [])
        try:
            config.save(config.get_path(), cfg_ref)
            if reload_cb:
                reload_cb()
            return jsonify(ok=True)
        except Exception as e:
            log.error("Failed to save config: %s", e)
            return jsonify(error=str(e)), 500

    @app.route("/api/test-connection", methods=["POST"])
    def test_conn():
        if not session.get("logged_in"):
            return jsonify(error="Unauthorized"), 401
        data = request.get_json(force=True)
        ok, msg = ha_client.test_connection(
            data.get("ha_url", ""), data.get("ha_token", "")
        )
        return jsonify(ok=ok, message=msg)

    return app


def start(port: int, cfg: dict, reload_cb=None) -> None:
    """Start the web config server in a background daemon thread."""
    global _app, _thread, _cfg, _reload_callback
    _cfg = cfg
    _reload_callback = reload_cb
    _app = _create_app(cfg, reload_cb)

    _thread = threading.Thread(
        target=lambda: _app.run(host="0.0.0.0", port=port, use_reloader=False),
        daemon=True,
    )
    _thread.start()
    log.info("Config server started on port %d", port)
