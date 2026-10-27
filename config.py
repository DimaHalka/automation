import json
import os
from pathlib import Path
from typing import Any, Dict, List

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
SETTINGS_FILE = DATA_DIR / "settings.json"
LINKS_FILE = DATA_DIR / "links.json"
SPRINT_TASKS_DIR = BASE_DIR / "sprint-tasks"

DEFAULT_SETTINGS: Dict[str, Any] = {
    "organization": os.environ.get("ADO_ORG", "your-org"),
    "project": os.environ.get("ADO_PROJECT", "YourProject"),
    "repository_id": os.environ.get("ADO_REPO", "YourRepo"),
    "target_branch": "master",
    "query_id": os.environ.get("ADO_QUERY_ID", ""),
    "user_prefix": os.environ.get("USER_PREFIX", os.environ.get("USERNAME", "user").lower()),
    "branch_prefix": "feature",
    "git_roots": [
        str(Path.home() / "Git"),
        r"D:\Git",
    ],
    "clang_format_path": "clang-format.exe",
    "pr_watch_interval": 60,
    "pr_watch_notify": False,
    "pr_watch_sound": True,
    "pr_watch_include_drafts": False,
}


def load_settings() -> Dict[str, Any]:
    """Завантажує налаштування з data/settings.json із дефолтними значеннями."""
    settings = dict(DEFAULT_SETTINGS)
    if SETTINGS_FILE.exists():
        try:
            loaded = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                settings.update(loaded)
        except Exception:
            pass
    return settings


def save_settings(new_settings: Dict[str, Any]) -> None:
    """Зберігає налаштування у data/settings.json."""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    SETTINGS_FILE.write_text(
        json.dumps(new_settings, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def get_git_roots(settings: Dict[str, Any] | None = None) -> List[Path]:
    """Повертає список кореневих папок Git як об'єкти Path."""
    cfg = settings or load_settings()
    roots = cfg.get("git_roots", DEFAULT_SETTINGS["git_roots"])
    return [Path(r) for r in roots if r]


def load_links() -> List[Dict[str, str]]:
    """Завантажує список швидких посилань із data/links.json."""
    if LINKS_FILE.exists():
        try:
            data = json.loads(LINKS_FILE.read_text(encoding="utf-8"))
            if isinstance(data, list):
                return data
        except Exception:
            pass
    return []


def save_links(links: List[Dict[str, str]]) -> None:
    """Зберігає список швидких посилань у data/links.json."""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    LINKS_FILE.write_text(
        json.dumps(links, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
