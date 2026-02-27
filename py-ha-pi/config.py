"""Configuration management — load/save/reload the JSON config file."""

import json
import os
import re

DEFAULT_CONFIG_PATH = "/etc/ha_lights.conf"
MAX_LIGHTS = 16

_config_path: str = DEFAULT_CONFIG_PATH
_current_config: dict | None = None


def _validate_entity_id(eid: str) -> bool:
    return bool(re.match(r"^[a-zA-Z0-9_]+\.[a-zA-Z0-9_]+$", eid))


def _validate_label(label: str) -> bool:
    return 0 < len(label) <= 31


def load(path: str) -> dict:
    """Load and validate config from a JSON file."""
    with open(path, "r") as f:
        data = json.load(f)

    cfg = {
        "ha_url": data.get("ha_url", ""),
        "ha_token": data.get("ha_token", ""),
        "web_password": data.get("web_password", ""),
        "lights": [],
    }

    for i, light in enumerate(data.get("lights", [])[:MAX_LIGHTS]):
        eid = light.get("entity_id", "")
        label = light.get("label", "")
        icon = light.get("icon", "bulb")

        if not _validate_entity_id(eid):
            raise ValueError(f"Light {i}: invalid entity_id '{eid}'")
        if not _validate_label(label):
            raise ValueError(f"Light {i}: invalid label '{label}'")

        cfg["lights"].append({"entity_id": eid, "label": label, "icon": icon})

    return cfg


def save(path: str, cfg: dict) -> None:
    """Save config as JSON."""
    with open(path, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")


def set_path(path: str) -> None:
    global _config_path
    _config_path = path


def get_path() -> str:
    return _config_path


def reload() -> dict:
    """Reload config from disk and return it."""
    global _current_config
    _current_config = load(_config_path)
    return _current_config


def get_current() -> dict | None:
    return _current_config
