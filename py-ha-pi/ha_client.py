"""Home Assistant REST API client using requests."""

import logging
import requests

log = logging.getLogger("ha_client")

_base_url: str = ""
_token: str = ""
_session: requests.Session | None = None


def init(base_url: str, token: str) -> None:
    global _base_url, _token, _session
    _base_url = base_url.rstrip("/")
    _token = token
    _session = requests.Session()
    _session.headers.update({
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    })
    _session.timeout = 10


def get_state(entity_id: str) -> str:
    """Fetch entity state. Returns 'on', 'off', or 'unknown'."""
    if not _session:
        return "unknown"
    try:
        r = _session.get(f"{_base_url}/api/states/{entity_id}", timeout=10)
        if r.status_code >= 400:
            log.warning("GET %s returned %d", entity_id, r.status_code)
            return "unknown"
        return r.json().get("state", "unknown")
    except Exception as e:
        log.error("Failed to get state for %s: %s", entity_id, e)
        return "unknown"


def toggle_light(entity_id: str) -> bool:
    """Toggle a light. Returns True on success."""
    if not _session:
        return False

    current = get_state(entity_id)
    service = "turn_off" if current == "on" else "turn_on"
    domain = entity_id.split(".")[0] if "." in entity_id else "light"

    try:
        r = _session.post(
            f"{_base_url}/api/services/{domain}/{service}",
            json={"entity_id": entity_id},
            timeout=10,
        )
        if r.status_code >= 400:
            log.error("Toggle %s failed: HTTP %d", entity_id, r.status_code)
            return False
        return True
    except Exception as e:
        log.error("Toggle %s failed: %s", entity_id, e)
        return False


def poll_all(lights: list[dict]) -> dict[str, str]:
    """Poll all lights, return {entity_id: state} dict."""
    states = {}
    for light in lights:
        eid = light["entity_id"]
        states[eid] = get_state(eid)
    return states


def test_connection(url: str, token: str) -> tuple[bool, str]:
    """Test HA connectivity. Returns (ok, message)."""
    try:
        r = requests.get(
            f"{url.rstrip('/')}/api/",
            headers={"Authorization": f"Bearer {token}"},
            timeout=10,
        )
        if r.status_code == 200:
            return True, "Connected successfully"
        elif r.status_code == 401:
            return False, "URL reachable but token is invalid (401)"
        else:
            return False, f"Unexpected response (HTTP {r.status_code})"
    except Exception as e:
        return False, f"Cannot reach Home Assistant: {e}"


def cleanup() -> None:
    global _session
    if _session:
        _session.close()
        _session = None
