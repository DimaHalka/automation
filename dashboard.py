#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Automation Dashboard (NiceGUI) — єдиний десктоп/веб інтерфейс для скриптів автоматизації:
  • Sprint Tasks (sync-sprint.py)
  • Task Setup & Environments (task-setup.py, create-pr.py)
  • Keep Space (keep-space.py)
  • PR Monitor (watch-pr.py)
  • Quick Links & Settings (data/links.json, data/settings.json)
"""

import asyncio
import importlib.util
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

from nicegui import app, run, ui

from config import (
    BASE_DIR,
    SPRINT_TASKS_DIR,
    get_git_roots,
    load_links,
    load_settings,
    save_links,
    save_settings,
)


def _import_script(name: str, filename: str):
    """Динамічно імпортує скрипт із дефісом у назві (наприклад, watch-pr.py)."""
    spec = importlib.util.spec_from_file_location(name, str(BASE_DIR / filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


keep_space_mod = _import_script("keep_space", "keep-space.py")
watch_pr_mod = _import_script("watch_pr", "watch-pr.py")


# ================= ДОПОМІЖНІ ФУНКЦІЇ ДАНИХ =================

def get_drives_info() -> List[Dict[str, Any]]:
    """Повертає інформацію про вільне місце на дисках із git_roots."""
    cfg = load_settings()
    roots = get_git_roots(cfg)
    drives = []
    seen = set()
    for r in roots:
        drive = (r.anchor or str(r)[:3]).upper()
        if drive in seen or not os.path.exists(drive):
            continue
        seen.add(drive)
        try:
            usage = shutil.disk_usage(drive)
            total_gb = usage.total / (1024**3)
            free_gb = usage.free / (1024**3)
            used_pct = ((usage.total - usage.free) / usage.total) * 100 if usage.total else 0
            drives.append({
                "drive": drive,
                "root": str(r),
                "total_gb": round(total_gb, 1),
                "free_gb": round(free_gb, 1),
                "used_pct": round(used_pct, 1),
            })
        except Exception:
            pass
    drives.sort(key=lambda d: d["free_gb"], reverse=True)
    return drives


def scan_task_folders_map() -> Dict[int, Path]:
    """Шукає локальні папки задач у git_roots та зіставляє їх із ID задач."""
    mapping: Dict[int, Path] = {}
    for root in get_git_roots():
        if not root.exists():
            continue
        try:
            for entry in root.iterdir():
                if not entry.is_dir() or entry.name.startswith("."):
                    continue
                for md_file in entry.glob("*_*.md"):
                    m = re.match(r"^(\d+)_", md_file.name)
                    if m:
                        mapping[int(m.group(1))] = entry
                        break
        except Exception:
            pass
    return mapping


def load_sprint_tasks() -> List[Dict[str, Any]]:
    """Читає всі .md файли з папки sprint-tasks/ та збагачує їх локальними папками."""
    if not SPRINT_TASKS_DIR.exists():
        return []

    folders_map = scan_task_folders_map()
    tasks = []

    for md_path in sorted(SPRINT_TASKS_DIR.glob("*.md")):
        try:
            content = md_path.read_text(encoding="utf-8")
        except Exception:
            continue

        task_id = 0
        task_type = "Task"
        title = md_path.stem
        state = "Unknown"
        priority = "N/A"
        url = ""

        m_title = re.search(r"^#\s*\[(\w+)\s*#(\d+)\]\s*(.*)$", content, re.MULTILINE)
        if m_title:
            task_type = m_title.group(1).strip()
            task_id = int(m_title.group(2))
            title = m_title.group(3).strip()

        m_state = re.search(r"-\s*\*\*State:\*\*\s*([^\n\r]+)", content)
        if m_state:
            state = m_state.group(1).strip()

        m_prio = re.search(r"-\s*\*\*Priority:\*\*\s*([^\n\r]+)", content)
        if m_prio:
            priority = m_prio.group(1).strip()

        m_url = re.search(r"-\s*\*\*URL:\*\*\s*([^\n\r]+)", content)
        if m_url:
            url = m_url.group(1).strip()

        folder = folders_map.get(task_id)
        tasks.append({
            "id": task_id,
            "type": task_type,
            "title": title,
            "state": state,
            "priority": priority,
            "url": url,
            "md_path": str(md_path.relative_to(BASE_DIR)),
            "abs_md_path": str(md_path),
            "content": content,
            "folder": str(folder) if folder else "",
        })

    state_order = {"Active": 0, "Proposed": 1, "New": 2, "Resolved": 3, "Closed": 4}
    tasks.sort(key=lambda t: (state_order.get(t["state"], 9), -t["id"]))
    return tasks


def scan_workspaces_sync() -> List[Dict[str, Any]]:
    """Сканує всі робочі папки у git_roots та рахує їхній розмір (для Keep Space)."""
    folders = []
    for root in get_git_roots():
        if not root.exists():
            continue
        try:
            for entry in root.iterdir():
                if entry.is_dir() and not entry.name.startswith("."):
                    folders.append(entry)
        except Exception:
            pass

    results = []
    for f in folders:
        info = keep_space_mod.get_task_info_from_folder(f)
        size_gb = keep_space_mod.get_dir_size_gb(f)
        state = info.get("state", "Unknown")
        is_closed = state.lower() in keep_space_mod.CLOSED_STATES
        results.append({
            "name": f.name,
            "path": str(f),
            "drive": f.anchor or str(f)[:3],
            "size_gb": round(size_gb, 2),
            "task_id": info.get("id") or "",
            "task_type": info.get("type", "Task"),
            "task_title": info.get("title", f.name),
            "state": state,
            "is_closed": is_closed,
        })

    results.sort(key=lambda x: (not x["is_closed"], -x["size_gb"]))
    return results


def fetch_pr_snapshots_sync(include_drafts: bool = False) -> Dict[int, Dict[str, Any]]:
    """Синхронно опитує Azure DevOps API для активних PR (виконується в io_bound потоці)."""
    cfg = load_settings()
    watch_pr_mod.ORGANIZATION = cfg["organization"]
    watch_pr_mod.PROJECT = cfg["project"]
    watch_pr_mod.REPOSITORY_ID = cfg["repository_id"]
    watch_pr_mod.PAT = os.environ.get("ADO_PAT", "")
    watch_pr_mod.AUTH = watch_pr_mod.HTTPBasicAuth("", watch_pr_mod.PAT)

    user_id = watch_pr_mod.get_current_user_id()
    raw_prs = watch_pr_mod.get_active_pull_requests(user_id, include_drafts=include_drafts)
    snaps = {}
    for pr in raw_prs:
        snaps[pr["pullRequestId"]] = watch_pr_mod.parse_pr_snapshot(pr)
    return snaps


def open_in_explorer(path_str: str) -> None:
    """Відкриває папку у Провіднику Windows."""
    p = Path(path_str)
    if p.exists():
        os.startfile(str(p))
    else:
        ui.notify(f"Папку не знайдено: {path_str}", type="warning")


def open_in_vscode(path_str: str) -> None:
    """Відкриває папку у VS Code або Antigravity."""
    p = Path(path_str)
    if not p.exists():
        ui.notify(f"Папку не знайдено: {path_str}", type="warning")
        return
    for cmd in ("agy", "code"):
        if shutil.which(cmd):
            subprocess.Popen([cmd, str(p)], shell=True)
            ui.notify(f"Відкриваю у {cmd}: {p.name}", type="positive")
            return
    os.startfile(str(p))


# ================= ГЛОБАЛЬНИЙ СТАН UI =================

class DashboardState:
    def __init__(self):
        cfg = load_settings()
        self.current_tab: str = "sprint"
        self.running_proc: Optional[asyncio.subprocess.Process] = None
        self.is_busy: bool = False

        # Для передачі обраної таски зі Sprint у Task Setup
        self.setup_md_path: str = ""
        self.setup_task_name: str = ""

        # PR Monitor стан
        self.pr_monitoring_active: bool = True
        self.pr_interval: int = int(cfg.get("pr_watch_interval", 60))
        self.pr_notify: bool = bool(cfg.get("pr_watch_notify", True))
        self.pr_sound: bool = bool(cfg.get("pr_watch_sound", True))
        self.pr_include_drafts: bool = bool(cfg.get("pr_watch_include_drafts", False))
        self.pr_snapshots: Dict[int, Dict[str, Any]] = {}
        self.pr_events: List[Dict[str, str]] = []
        self.pr_last_check: str = "Ще не перевірялося"
        self.pr_error: str = ""
        self.pr_checking: bool = False


state = DashboardState()


# ================= СТИЛІ ANTIGRAVITY DARK =================

ANTIGRAVITY_CSS = """
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap');

body {
    background-color: #0b0d12 !important;
    color: #e2e8f0 !important;
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif !important;
}

.agy-sidebar {
    background: linear-gradient(180deg, #0f1219 0%, #0b0d12 100%);
    border-right: 1px solid #1e2433;
}

.agy-header {
    background: rgba(15, 18, 25, 0.85);
    backdrop-filter: blur(12px);
    border-bottom: 1px solid #1e2433;
}

.agy-card {
    background-color: #121620 !important;
    border: 1px solid #1e2536 !important;
    border-radius: 10px !important;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.25) !important;
    transition: border-color 0.15s ease, transform 0.15s ease;
}

.agy-card-hover:hover {
    border-color: #3b82f6 !important;
}

.agy-nav-btn {
    border-radius: 8px !important;
    justify-content: flex-start !important;
    padding: 10px 14px !important;
    font-weight: 500 !important;
    color: #94a3b8 !important;
    transition: all 0.15s ease !important;
}

.agy-nav-btn:hover {
    background-color: #181e2c !important;
    color: #f8fafc !important;
}

.agy-nav-btn-active {
    background: rgba(59, 130, 246, 0.14) !important;
    color: #60a5fa !important;
    border-left: 3px solid #3b82f6 !important;
}

.agy-mono {
    font-family: 'JetBrains Mono', monospace !important;
}

.agy-terminal {
    background-color: #080a0f !important;
    border: 1px solid #1e2433 !important;
    border-radius: 8px !important;
    font-family: 'JetBrains Mono', monospace !important;
    font-size: 12.5px !important;
    color: #cbd5e1 !important;
}
"""


def state_badge_color(task_state: str) -> str:
    st = (task_state or "").lower()
    if st == "active":
        return "blue-6"
    if st in ("proposed", "new"):
        return "amber-7"
    if st in ("resolved", "closed", "done", "completed"):
        return "emerald-7"
    return "grey-7"


# ================= ЗАПУСК ПІДПРОЦЕСІВ З ЖИВИМ ЛОГОМ =================

async def run_script_into_log(
    cmd: List[str],
    log_view: ui.log,
    status_label: Optional[ui.label] = None,
    cwd: Optional[Path] = None,
    on_done=None,
):
    """Запускає Python-скрипт асинхронно і стрімить stdout/stderr у реальному часі в ui.log."""
    if state.is_busy:
        ui.notify("Інший процес вже виконується. Зачекайте або зупиніть його.", type="warning")
        return

    state.is_busy = True
    log_view.clear()
    cmd_str = " ".join(cmd)
    log_view.push(f"[{datetime.now().strftime('%H:%M:%S')}] $ {cmd_str}")
    if status_label:
        status_label.set_text("⏳ Виконується...")

    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            cwd=str(cwd or BASE_DIR),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            env=env,
        )
        state.running_proc = proc

        assert proc.stdout is not None
        while True:
            line = await proc.stdout.readline()
            if not line:
                break
            decoded = line.decode("utf-8", errors="replace").rstrip("\r\n")
            # Прибираємо ANSI-кольори, якщо вони є
            clean_line = re.sub(r"\x1b\[[0-9;]*m", "", decoded)
            log_view.push(clean_line)

        rc = await proc.wait()
        if rc == 0:
            log_view.push(f"\n✅ Завершено успішно (код 0)")
            if status_label:
                status_label.set_text("✅ Готово")
            ui.notify("Команду успішно виконано!", type="positive")
        else:
            log_view.push(f"\n❌ Завершено з помилкою (код {rc})")
            if status_label:
                status_label.set_text(f"❌ Помилка (код {rc})")
            ui.notify(f"Процес завершився з кодом {rc}", type="negative")

    except Exception as e:
        log_view.push(f"\n❌ Виняток: {e}")
        if status_label:
            status_label.set_text("❌ Помилка")
        ui.notify(f"Помилка запуску: {e}", type="negative")
    finally:
        state.running_proc = None
        state.is_busy = False
        if on_done:
            res = on_done()
            if asyncio.iscoroutine(res):
                await res


def stop_running_proc():
    if state.running_proc and state.running_proc.returncode is None:
        try:
            state.running_proc.terminate()
            ui.notify("Процес зупинено користувачем.", type="warning")
        except Exception as e:
            ui.notify(f"Не вдалося зупинити процес: {e}", type="negative")


# ================= ГОЛОВНИЙ ІНТЕРФЕЙС =================

@ui.page("/")
def index_page():
    ui.dark_mode().enable()
    ui.add_head_html(f"<style>{ANTIGRAVITY_CSS}</style>")
    ui.page_title("Antigravity Automation Hub")

    nav_buttons: Dict[str, ui.button] = {}
    pr_header_badge: Optional[ui.badge] = None

    def switch_tab(tab_name: str):
        state.current_tab = tab_name
        for k, btn in nav_buttons.items():
            if k == tab_name:
                btn.classes(add="agy-nav-btn-active")
            else:
                btn.classes(remove="agy-nav-btn-active")
        main_content.refresh()

    def jump_to_setup(task: Dict[str, Any]):
        state.setup_md_path = task["md_path"]
        # Пропонуємо коротку назву з перших 2 слів заголовку
        words = re.findall(r"[A-Za-z0-9]+", task["title"])
        suggested = "-".join(words[:2]) if words else f"Task-{task['id']}"
        state.setup_task_name = f"{task['id']}-{suggested}"
        switch_tab("setup")

    # ---------------- HEADER ----------------
    with ui.header().classes("agy-header items-center justify-between px-6 py-3"):
        with ui.row().classes("items-center gap-3"):
            ui.icon("bolt", color="blue-400", size="26px")
            ui.label("Automation Hub").classes("text-lg font-bold tracking-tight text-white")
            cfg = load_settings()
            ui.badge(f"{cfg['organization']} / {cfg['project']}", color="blue-9").classes(
                "agy-mono text-xs px-2 py-0.5"
            )

        with ui.row().classes("items-center gap-4"):
            # Індикатор ADO_PAT
            has_pat = bool(os.environ.get("ADO_PAT"))
            with ui.row().classes("items-center gap-1.5 bg-[#141824] px-3 py-1 rounded-full border border-[#1e2536]"):
                ui.icon(
                    "verified_user" if has_pat else "gpp_maybe",
                    color="emerald-400" if has_pat else "red-400",
                    size="16px",
                )
                ui.label("ADO_PAT: OK" if has_pat else "ADO_PAT: Відсутній!").classes(
                    "text-xs agy-mono " + ("text-emerald-400" if has_pat else "text-red-400")
                )

            # Індикатор PR Watcher
            with ui.row().classes(
                "items-center gap-2 bg-[#141824] px-3 py-1 rounded-full border border-[#1e2536] cursor-pointer"
            ).on("click", lambda: switch_tab("pr")):
                ui.icon("radar", color="cyan-400", size="16px")
                pr_header_badge = ui.label("PR Monitor: активний").classes("text-xs agy-mono text-slate-300")

    # ---------------- LAYOUT: SIDEBAR + MAIN ----------------
    with ui.row().classes("w-full min-h-[calc(100vh-58px)] no-wrap gap-0"):
        # Лівий сайдбар у стилі Antigravity
        with ui.column().classes("agy-sidebar w-64 shrink-0 p-4 justify-between min-h-[calc(100vh-58px)]"):
            with ui.column().classes("w-full gap-1.5"):
                ui.label("РОБОЧИЙ ПРОСТІР").classes("text-[11px] font-semibold text-slate-500 px-3 mb-1 tracking-wider")

                nav_items = [
                    ("sprint", "view_kanban", "Таски спринту"),
                    ("setup", "terminal", "Робоче оточення"),
                    ("space", "cleaning_services", "Keep Space (Диск)"),
                    ("pr", "merge", "Моніторинг PR"),
                    ("links", "bookmarks", "Лінки та Налаштування"),
                ]
                for key, icon_name, label_text in nav_items:
                    btn = (
                        ui.button(label_text, icon=icon_name, on_click=lambda k=key: switch_tab(k))
                        .props("flat no-caps align=left")
                        .classes("w-full agy-nav-btn" + (" agy-nav-btn-active" if state.current_tab == key else ""))
                    )
                    nav_buttons[key] = btn

            # Віджет дисків унизу сайдбару
            with ui.column().classes("w-full gap-2 pt-4 border-t border-[#1e2433]"):
                ui.label("ДИСКИ (GIT ROOTS)").classes("text-[11px] font-semibold text-slate-500 px-1 tracking-wider")
                for d in get_drives_info():
                    with ui.column().classes("w-full bg-[#121620] p-2.5 rounded-lg border border-[#1e2536] gap-1"):
                        with ui.row().classes("w-full justify-between items-center"):
                            ui.label(f"{d['drive']} ({d['root']})").classes("text-xs font-medium agy-mono text-slate-300")
                            ui.label(f"{d['free_gb']} GB вільно").classes("text-xs agy-mono text-blue-400")
                        ui.linear_progress(value=d["used_pct"] / 100.0, show_value=False, color="blue-5").classes(
                            "h-1.5 rounded"
                        )

        # Основна частина екрану
        with ui.column().classes("flex-1 p-6 overflow-y-auto max-w-full"):

            @ui.refreshable
            def main_content():
                if state.current_tab == "sprint":
                    render_sprint_page(jump_to_setup)
                elif state.current_tab == "setup":
                    render_setup_page()
                elif state.current_tab == "space":
                    render_keep_space_page()
                elif state.current_tab == "pr":
                    render_pr_monitor_page()
                elif state.current_tab == "links":
                    render_links_and_settings_page()

            main_content()

    # ---------------- ФОНОВИЙ ТАЙМЕР ДЛЯ PR MONITOR ----------------
    async def background_pr_tick():
        if not state.pr_monitoring_active or state.pr_checking:
            if pr_header_badge:
                pr_header_badge.set_text(
                    "PR Monitor: пауза" if not state.pr_monitoring_active else "PR Monitor: оновлення..."
                )
            return

        if not os.environ.get("ADO_PAT"):
            state.pr_error = "Змінна середовища ADO_PAT не встановлена"
            return

        state.pr_checking = True
        try:
            new_snaps = await run.io_bound(fetch_pr_snapshots_sync, state.pr_include_drafts)
            now_str = datetime.now().strftime("%H:%M:%S")

            if state.pr_snapshots:
                raw_events = watch_pr_mod.compare_snapshots(
                    state.pr_snapshots,
                    new_snaps,
                    notify_enabled=state.pr_notify,
                    sound_enabled=state.pr_sound,
                )
                for ev in raw_events:
                    clean_ev = re.sub(r"\x1b\[[0-9;]*m", "", ev)
                    state.pr_events.insert(0, {"time": now_str, "text": clean_ev})
                state.pr_events = state.pr_events[:50]

            state.pr_snapshots = new_snaps
            state.pr_last_check = now_str
            state.pr_error = ""

            if pr_header_badge:
                pr_header_badge.set_text(f"PRs: {len(new_snaps)} активних ({now_str})")

            if state.current_tab == "pr":
                main_content.refresh()
        except Exception as e:
            state.pr_error = str(e)
            if pr_header_badge:
                pr_header_badge.set_text("PR Monitor: помилка")
        finally:
            state.pr_checking = False

    # Перевіряємо PR одразу через 2 секунди після старту і далі кожні N секунд
    ui.timer(2.0, background_pr_tick, once=True)
    ui.timer(float(max(15, state.pr_interval)), background_pr_tick)


# ================= 1. СТОРІНКА: SPRINT TASKS =================

def render_sprint_page(jump_to_setup_cb):
    tasks = load_sprint_tasks()
    selected_task: Dict[str, Any] = tasks[0] if tasks else {}
    filter_state = {"search": "", "status": "Всі"}

    with ui.row().classes("w-full justify-between items-center mb-4"):
        with ui.column().classes("gap-0.5"):
            ui.label("Таски на спринті").classes("text-2xl font-bold text-white")
            ui.label(
                f"Локальний кеш у папці sprint-tasks/ ({len(tasks)} задач знайдено)"
            ).classes("text-sm text-slate-400")

        with ui.row().classes("items-center gap-3"):
            sync_status = ui.label("").classes("text-xs agy-mono text-slate-400")
            sync_btn = ui.button("Sync Sprint з ADO", icon="sync").props("unelevated color=blue-6 no-caps")

    # Панель живого логу для sync-sprint (прихована за замовчуванням, розгортається при синху)
    with ui.expansion("Лог синхронізації (sync-sprint.py)", icon="terminal").classes(
        "w-full agy-card mb-4"
    ) as sync_exp:
        sync_log = ui.log(max_lines=200).classes("w-full h-44 agy-terminal p-3")

    async def trigger_sync():
        sync_exp.open()
        await run_script_into_log(
            [sys.executable, str(BASE_DIR / "sync-sprint.py")],
            sync_log,
            status_label=sync_status,
            on_done=lambda: tasks_view.refresh(),
        )

    sync_btn.on_click(trigger_sync)

    @ui.refreshable
    def tasks_view():
        all_tasks = load_sprint_tasks()
        counts = {"Всі": len(all_tasks), "Active": 0, "Proposed": 0, "Resolved": 0, "Closed": 0}
        for t in all_tasks:
            st = t["state"]
            counts[st] = counts.get(st, 0) + 1

        # Фільтри
        with ui.row().classes("w-full items-center justify-between gap-4 mb-4"):
            with ui.row().classes("items-center gap-2"):
                for st_name in ["Всі", "Active", "Proposed", "Resolved", "Closed"]:
                    if st_name != "Всі" and counts.get(st_name, 0) == 0:
                        continue
                    is_sel = filter_state["status"] == st_name
                    ui.button(
                        f"{st_name} ({counts.get(st_name, 0)})",
                        on_click=lambda s=st_name: (filter_state.update({"status": s}), tasks_view.refresh()),
                    ).props(
                        f"{'unelevated color=blue-6' if is_sel else 'outline color=grey-7'} size=sm no-caps"
                    )

            search_input = (
                ui.input(placeholder="Пошук за #ID або назвою...", value=filter_state["search"])
                .props("outlined dense dark clearable")
                .classes("w-72")
            )
            search_input.on(
                "update:model-value",
                lambda e: (filter_state.update({"search": e.args or ""}), list_and_preview.refresh()),
            )

        @ui.refreshable
        def list_and_preview():
            nonlocal selected_task
            q = (filter_state["search"] or "").strip().lower()
            st_filter = filter_state["status"]

            filtered = [
                t
                for t in all_tasks
                if (st_filter == "Всі" or t["state"] == st_filter)
                and (not q or q in str(t["id"]).lower() or q in t["title"].lower() or q in t["type"].lower())
            ]

            if not filtered:
                with ui.card().classes("w-full agy-card p-8 items-center justify-center"):
                    ui.icon("inbox", size="42px", color="slate-600")
                    ui.label("Задач за цим фільтром не знайдено. Натисніть 'Sync Sprint з ADO'.").classes(
                        "text-slate-400 mt-2"
                    )
                return

            if not selected_task or selected_task["id"] not in {t["id"] for t in filtered}:
                selected_task = filtered[0]

            with ui.row().classes("w-full gap-4 items-start no-wrap"):
                # Ліва колонка: список карток задач
                with ui.column().classes("w-7/12 gap-2.5"):
                    for t in filtered:
                        is_current = selected_task and selected_task["id"] == t["id"]
                        border_cls = "border-blue-500 bg-[#161c2b]" if is_current else ""
                        with ui.card().classes(
                            f"w-full agy-card agy-card-hover p-3.5 cursor-pointer {border_cls}"
                        ).on("click", lambda _, item=t: select_task(item)):
                            with ui.row().classes("w-full justify-between items-start no-wrap gap-3"):
                                with ui.column().classes("gap-1 flex-1"):
                                    with ui.row().classes("items-center gap-2"):
                                        ui.badge(t["state"], color=state_badge_color(t["state"])).classes("text-xs")
                                        ui.label(f"#{t['id']} • {t['type']}").classes(
                                            "text-xs agy-mono text-slate-400 font-semibold"
                                        )
                                        if t["priority"] != "N/A":
                                            ui.label(f"P{t['priority']}").classes(
                                                "text-xs agy-mono text-amber-400 bg-amber-950/50 px-1.5 rounded"
                                            )
                                    ui.label(t["title"]).classes("text-sm font-medium text-slate-100 leading-snug")

                                    if t["folder"]:
                                        with ui.row().classes("items-center gap-1 mt-1"):
                                            ui.icon("folder_special", size="15px", color="emerald-400")
                                            ui.label(t["folder"]).classes("text-xs agy-mono text-emerald-400")

                                # Кнопки швидких дій на картці
                                with ui.row().classes("items-center gap-1 shrink-0"):
                                    if t["folder"]:
                                        ui.button(
                                            icon="folder_open",
                                            on_click=lambda _, p=t["folder"]: open_in_explorer(p),
                                        ).props("flat round dense size=sm color=emerald-4").tooltip(
                                            "Відкрити робочу папку в Explorer"
                                        )
                                        ui.button(
                                            icon="code",
                                            on_click=lambda _, p=t["folder"]: open_in_vscode(p),
                                        ).props("flat round dense size=sm color=blue-4").tooltip(
                                            "Відкрити у VS Code / Antigravity"
                                        )
                                    else:
                                        ui.button(
                                            "Setup Env",
                                            icon="rocket_launch",
                                            on_click=lambda _, item=t: jump_to_setup_cb(item),
                                        ).props("outline dense size=sm color=blue-4 no-caps").classes("px-2")

                                    if t["url"]:
                                        ui.button(
                                            icon="open_in_new",
                                            on_click=lambda _, u=t["url"]: ui.navigate.to(u, new_tab=True),
                                        ).props("flat round dense size=sm color=grey-5").tooltip("Відкрити в ADO")

                # Права колонка: прев'ю Markdown-файлу обраної таски
                with ui.card().classes("w-5/12 agy-card p-5 sticky top-4"):
                    if selected_task:
                        with ui.row().classes("w-full justify-between items-center pb-3 mb-3 border-b border-[#1e2536]"):
                            with ui.row().classes("items-center gap-2"):
                                ui.icon("description", color="blue-400", size="20px")
                                ui.label(selected_task["md_path"]).classes("text-xs agy-mono text-slate-400")
                            with ui.row().classes("gap-2"):
                                if not selected_task["folder"]:
                                    ui.button(
                                        "Створити оточення",
                                        icon="rocket_launch",
                                        on_click=lambda: jump_to_setup_cb(selected_task),
                                    ).props("unelevated dense color=blue-6 no-caps size=sm").classes("px-2.5")
                                if selected_task["url"]:
                                    ui.button(
                                        "ADO",
                                        icon="open_in_new",
                                        on_click=lambda: ui.navigate.to(selected_task["url"], new_tab=True),
                                    ).props("outline dense color=grey-5 no-caps size=sm")

                        ui.markdown(selected_task["content"]).classes(
                            "text-sm text-slate-200 max-h-[65vh] overflow-y-auto pr-2"
                        )

        def select_task(item: Dict[str, Any]):
            nonlocal selected_task
            selected_task = item
            list_and_preview.refresh()

        list_and_preview()

    tasks_view()


# ================= 2. СТОРІНКА: TASK SETUP & ENVIRONMENTS =================

def render_setup_page():
    cfg = load_settings()
    tasks = load_sprint_tasks()
    drives = get_drives_info()
    best_drive_info = drives[0] if drives else {"drive": "C:\\", "root": str(Path.home() / "Git"), "free_gb": 0}

    with ui.column().classes("w-full gap-1 mb-4"):
        ui.label("Створення робочого оточення (task-setup)").classes("text-2xl font-bold text-white")
        ui.label(
            "Клонує репозиторій на найвільніший диск, ініціалізує субмодулі, створює гілку, генерує AI_PROMPT.md та запускає CMake.bat"
        ).classes("text-sm text-slate-400")

    with ui.row().classes("w-full gap-5 items-start no-wrap"):
        # Ліва картка: форма створення оточення
        with ui.card().classes("w-5/12 agy-card p-5 gap-4"):
            ui.label("Нове робоче оточення").classes("text-base font-semibold text-white")

            # Вибір .md файлу зі спринту
            md_options = {"": "— Без прив'язки до .md файлу —"}
            for t in tasks:
                md_options[t["md_path"]] = f"[{t['state']}] #{t['id']} — {t['title'][:55]}"

            md_select = (
                ui.select(
                    options=md_options,
                    value=state.setup_md_path if state.setup_md_path in md_options else "",
                    label="Задача зі спринту (.md файл)",
                )
                .props("outlined dark dense")
                .classes("w-full")
            )

            task_input = (
                ui.input(
                    label="Коротка назва папки / гілки (наприклад: Oste-Rotate)",
                    value=state.setup_task_name,
                    placeholder="12345-Short-Name",
                )
                .props("outlined dark dense")
                .classes("w-full agy-mono")
            )

            def on_md_change(e):
                val = e.value
                state.setup_md_path = val
                if val:
                    matched = next((t for t in tasks if t["md_path"] == val), None)
                    if matched and not task_input.value:
                        words = re.findall(r"[A-Za-z0-9]+", matched["title"])
                        short = "-".join(words[:2]) if words else "Task"
                        task_input.value = f"{matched['id']}-{short}"
                update_preview()

            md_select.on_value_change(on_md_change)
            task_input.on("update:model-value", lambda _: update_preview())

            # Прев'ю куди саме клонуватиметься і яка буде гілка
            with ui.column().classes("w-full bg-[#0b0d12] p-3.5 rounded-lg border border-[#1e2433] gap-1.5"):
                ui.label("Прев'ю параметрів:").classes("text-xs font-semibold text-slate-400")
                folder_preview = ui.label("").classes("text-xs agy-mono text-emerald-400")
                branch_preview = ui.label("").classes("text-xs agy-mono text-blue-400")

            def update_preview():
                t_name = (task_input.value or "<task-name>").strip()
                state.setup_task_name = task_input.value or ""
                target_folder = Path(best_drive_info["root"]) / t_name
                b_prefix = cfg.get("branch_prefix", cfg["repository_id"].lower())
                u_prefix = cfg.get("user_prefix", "user")
                branch = f"{b_prefix}/{u_prefix}/{t_name.lower()}"
                folder_preview.set_text(f"📂 Папка:  {target_folder} ({best_drive_info['free_gb']} GB вільно)")
                branch_preview.set_text(f"🌿 Гілка:  {branch}")

            update_preview()

            with ui.row().classes("w-full justify-between items-center pt-2"):
                status_lbl = ui.label("Готовий до запуску").classes("text-xs agy-mono text-slate-400")
                with ui.row().classes("gap-2"):
                    ui.button("Зупинити", icon="stop", on_click=stop_running_proc).props(
                        "outline color=red-5 no-caps dense"
                    )
                    run_btn = ui.button("Запустити task-setup", icon="play_arrow").props(
                        "unelevated color=blue-6 no-caps"
                    )

        # Права картка: Живий термінал виконання
        with ui.card().classes("w-7/12 agy-card p-5 gap-3"):
            with ui.row().classes("w-full justify-between items-center"):
                with ui.row().classes("items-center gap-2"):
                    ui.icon("terminal", color="emerald-400", size="20px")
                    ui.label("Живий вивід терміналу").classes("text-base font-semibold text-white")
                ui.button("Очистити", icon="delete_sweep", on_click=lambda: setup_log.clear()).props(
                    "flat dense size=sm color=grey-5 no-caps"
                )

            setup_log = ui.log(max_lines=500).classes("w-full h-80 agy-terminal p-3")

    async def start_task_setup():
        t_name = (task_input.value or "").strip()
        if not t_name:
            ui.notify("Введіть коротку назву задачі!", type="warning")
            return

        cmd = [sys.executable, str(BASE_DIR / "task-setup.py"), "-t", t_name]
        if md_select.value:
            cmd.extend(["-m", md_select.value])

        await run_script_into_log(cmd, setup_log, status_label=status_lbl)

    run_btn.on_click(start_task_setup)


# ================= 3. СТОРІНКА: KEEP SPACE =================

def render_keep_space_page():
    selected_paths: set[str] = set()
    workspaces_data: List[Dict[str, Any]] = []
    is_scanning = {"val": False}

    with ui.row().classes("w-full justify-between items-center mb-4"):
        with ui.column().classes("gap-0.5"):
            ui.label("Keep Space — Очищення робочих папок").classes("text-2xl font-bold text-white")
            ui.label(
                "Сканує всі робочі директорії в git_roots, звіряє статуси задач зі спринтом і дозволяє видалити закриті таски"
            ).classes("text-sm text-slate-400")

        with ui.row().classes("gap-2"):
            scan_btn = ui.button("Сканувати диски", icon="radar").props("outline color=blue-4 no-caps")
            select_closed_btn = ui.button("Вибрати всі закриті", icon="check_box").props(
                "outline color=emerald-4 no-caps"
            )
            delete_btn = ui.button("Видалити вибрані", icon="delete_forever").props("unelevated color=red-6 no-caps")

    @ui.refreshable
    def space_table_view():
        if is_scanning["val"]:
            with ui.card().classes("w-full agy-card p-10 items-center justify-center gap-3"):
                ui.spinner("dots", size="40px", color="blue-4")
                ui.label("Підрахунок розміру робочих папок у git_roots...").classes("text-slate-300")
            return

        if not workspaces_data:
            with ui.card().classes("w-full agy-card p-8 items-center justify-center"):
                ui.icon("folder_off", size="40px", color="slate-600")
                ui.label("Натисніть 'Сканувати диски', щоб проаналізувати зайняте місце.").classes("text-slate-400 mt-2")
            return

        total_gb = sum(w["size_gb"] for w in workspaces_data)
        selected_gb = sum(w["size_gb"] for w in workspaces_data if w["path"] in selected_paths)
        closed_gb = sum(w["size_gb"] for w in workspaces_data if w["is_closed"])

        # Картки статистики
        with ui.row().classes("w-full gap-4 mb-4"):
            with ui.card().classes("flex-1 agy-card p-4"):
                ui.label("Усього робочих папок").classes("text-xs text-slate-400")
                ui.label(f"{len(workspaces_data)} папок ({total_gb:.1f} GB)").classes(
                    "text-xl font-bold text-white agy-mono mt-1"
                )
            with ui.card().classes("flex-1 agy-card p-4"):
                ui.label("Закриті таски (можна звільнити)").classes("text-xs text-slate-400")
                ui.label(f"{closed_gb:.1f} GB").classes("text-xl font-bold text-emerald-400 agy-mono mt-1")
            with ui.card().classes("flex-1 agy-card p-4"):
                ui.label("Вибрано до видалення").classes("text-xs text-slate-400")
                ui.label(f"{len(selected_paths)} папок ({selected_gb:.1f} GB)").classes(
                    "text-xl font-bold text-red-400 agy-mono mt-1"
                )

        # Таблиця папок
        with ui.column().classes("w-full gap-2"):
            for w in workspaces_data:
                p = w["path"]
                is_checked = p in selected_paths
                row_border = "border-red-500/60 bg-red-950/10" if is_checked else ""
                with ui.card().classes(f"w-full agy-card p-3.5 {row_border}"):
                    with ui.row().classes("w-full items-center justify-between no-wrap gap-4"):
                        with ui.row().classes("items-center gap-3 flex-1"):
                            cb = ui.checkbox(
                                value=is_checked,
                                on_change=lambda e, path=p: toggle_path(path, e.value),
                            ).props("dark color=red-5")

                            with ui.column().classes("gap-0.5"):
                                with ui.row().classes("items-center gap-2"):
                                    ui.label(w["name"]).classes("text-sm font-semibold text-white agy-mono")
                                    ui.badge(w["state"], color=state_badge_color(w["state"])).classes("text-xs")
                                    if w["is_closed"]:
                                        ui.badge("Рекомендовано видалити", color="emerald-8").classes("text-[10px]")
                                ui.label(
                                    f"#{w['task_id']} {w['task_title']}" if w["task_id"] else w["path"]
                                ).classes("text-xs text-slate-400")

                        with ui.row().classes("items-center gap-3 shrink-0"):
                            ui.label(f"{w['size_gb']:.2f} GB").classes(
                                "text-sm font-bold agy-mono "
                                + ("text-amber-400" if w["size_gb"] > 10 else "text-slate-200")
                            )
                            ui.button(
                                icon="folder_open",
                                on_click=lambda _, path=p: open_in_explorer(path),
                            ).props("flat round dense size=sm color=grey-5").tooltip("Відкрити в Explorer")
                            ui.button(
                                icon="code",
                                on_click=lambda _, path=p: open_in_vscode(path),
                            ).props("flat round dense size=sm color=blue-4").tooltip("Відкрити у VS Code / Antigravity")

    def toggle_path(path_str: str, checked: bool):
        if checked:
            selected_paths.add(path_str)
        else:
            selected_paths.discard(path_str)
        space_table_view.refresh()

    async def do_scan():
        is_scanning["val"] = True
        space_table_view.refresh()
        res = await run.io_bound(scan_workspaces_sync)
        workspaces_data.clear()
        workspaces_data.extend(res)
        selected_paths.clear()
        # Автоматично відмічаємо закриті задачі
        for w in workspaces_data:
            if w["is_closed"]:
                selected_paths.add(w["path"])
        is_scanning["val"] = False
        space_table_view.refresh()

    def select_all_closed():
        for w in workspaces_data:
            if w["is_closed"]:
                selected_paths.add(w["path"])
        space_table_view.refresh()

    async def confirm_and_delete():
        if not selected_paths:
            ui.notify("Спочатку виберіть папки для видалення.", type="warning")
            return

        to_del = [w for w in workspaces_data if w["path"] in selected_paths]
        freed_gb = sum(w["size_gb"] for w in to_del)

        with ui.dialog() as dialog, ui.card().classes("agy-card p-6 w-[480px]"):
            ui.label("Підтвердження видалення").classes("text-lg font-bold text-white")
            ui.label(
                f"Ви дійсно хочете назавжди видалити {len(to_del)} папок (звільниться ~{freed_gb:.2f} GB)?"
            ).classes("text-sm text-slate-300 mt-1")
            with ui.column().classes("w-full bg-[#0b0d12] p-3 rounded border border-[#1e2433] my-3 max-h-40 overflow-y-auto"):
                for item in to_del:
                    ui.label(f"• {item['path']} ({item['size_gb']:.2f} GB)").classes("text-xs agy-mono text-red-300")

            with ui.row().classes("w-full justify-end gap-2"):
                ui.button("Скасувати", on_click=dialog.close).props("flat color=grey-5 no-caps")

                async def execute_delete():
                    dialog.close()
                    is_scanning["val"] = True
                    space_table_view.refresh()
                    for item in to_del:
                        target = Path(item["path"])
                        if target.exists():
                            await run.io_bound(
                                lambda t=target: shutil.rmtree(t, onerror=keep_space_mod.remove_readonly)
                            )
                    ui.notify(f"Видалено {len(to_del)} папок (~{freed_gb:.1f} GB звільнено)!", type="positive")
                    await do_scan()

                ui.button("Видалити назавжди", icon="delete_forever", on_click=execute_delete).props(
                    "unelevated color=red-6 no-caps"
                )

        dialog.open()

    scan_btn.on_click(do_scan)
    select_closed_btn.on_click(select_all_closed)
    delete_btn.on_click(confirm_and_delete)

    space_table_view()
    # Автоматично запускаємо сканування при першому відкритті вкладки
    ui.timer(0.1, do_scan, once=True)


# ================= 4. СТОРІНКА: PR MONITOR =================

def render_pr_monitor_page():
    with ui.row().classes("w-full justify-between items-center mb-4"):
        with ui.column().classes("gap-0.5"):
            ui.label("Моніторинг Pull Requests (watch-pr)").classes("text-2xl font-bold text-white")
            ui.label(
                f"Останнє опитування: {state.pr_last_check} • Автоматична перевірка кожні {state.pr_interval} сек"
            ).classes("text-sm text-slate-400")

        with ui.row().classes("items-center gap-3"):
            ui.switch(
                "Фоновий моніторинг",
                value=state.pr_monitoring_active,
                on_change=lambda e: setattr(state, "pr_monitoring_active", e.value),
            ).props("color=blue-5 dark")
            ui.switch(
                "Toast-сповіщення",
                value=state.pr_notify,
                on_change=lambda e: setattr(state, "pr_notify", e.value),
            ).props("color=emerald-5 dark")
            ui.switch(
                "Звук",
                value=state.pr_sound,
                on_change=lambda e: setattr(state, "pr_sound", e.value),
            ).props("color=amber-5 dark")

            async def manual_refresh():
                state.pr_checking = True
                try:
                    snaps = await run.io_bound(fetch_pr_snapshots_sync, state.pr_include_drafts)
                    state.pr_snapshots = snaps
                    state.pr_last_check = datetime.now().strftime("%H:%M:%S")
                    state.pr_error = ""
                    ui.notify(f"Оновлено! Активних PR: {len(snaps)}", type="positive")
                except Exception as e:
                    state.pr_error = str(e)
                    ui.notify(f"Помилка ADO: {e}", type="negative")
                finally:
                    state.pr_checking = False
                    pr_cards_view.refresh()

            ui.button("Оновити зараз", icon="refresh", on_click=manual_refresh).props(
                "unelevated color=blue-6 no-caps"
            )

    @ui.refreshable
    def pr_cards_view():
        if state.pr_error:
            with ui.card().classes("w-full agy-card border-red-500/50 p-4 mb-4"):
                with ui.row().classes("items-center gap-2 text-red-400"):
                    ui.icon("error_outline", size="22px")
                    ui.label(f"Помилка з'єднання з Azure DevOps: {state.pr_error}").classes("text-sm agy-mono")

        snaps = list(state.pr_snapshots.values())
        with ui.row().classes("w-full gap-5 items-start no-wrap"):
            # Ліва частина: картки активних PR
            with ui.column().classes("w-8/12 gap-4"):
                if not snaps:
                    with ui.card().classes("w-full agy-card p-10 items-center justify-center"):
                        ui.icon("check_circle_outline", size="44px", color="emerald-400")
                        ui.label("Немає активних Pull Requests (або триває перше завантаження).").classes(
                            "text-slate-300 mt-2"
                        )
                else:
                    for snap in snaps:
                        with ui.card().classes("w-full agy-card p-5 gap-3"):
                            with ui.row().classes("w-full justify-between items-start"):
                                with ui.column().classes("gap-1"):
                                    with ui.row().classes("items-center gap-2"):
                                        ui.badge(f"PR #{snap['id']}", color="blue-7").classes("agy-mono text-xs")
                                        ui.label(snap["title"]).classes("text-base font-bold text-white")
                                    ui.label(f"🌿 {snap['branch']}").classes("text-xs agy-mono text-cyan-400")
                                ui.button(
                                    "Відкрити в ADO",
                                    icon="open_in_new",
                                    on_click=lambda _, u=snap["url"]: ui.navigate.to(u, new_tab=True),
                                ).props("outline dense size=sm color=blue-4 no-caps")

                            # Секція Gated Builds
                            builds = snap.get("builds", {})
                            with ui.column().classes("w-full bg-[#0b0d12] p-3 rounded-lg border border-[#1e2433] gap-1.5"):
                                ui.label("Gated Builds").classes("text-xs font-semibold text-slate-400")
                                if not builds:
                                    ui.label("Немає налаштованих Gated білдів").classes("text-xs text-slate-500")
                                else:
                                    for b_name, b_st in sorted(builds.items()):
                                        st_l = (b_st or "").lower()
                                        color = (
                                            "text-emerald-400"
                                            if st_l in ("approved", "succeeded")
                                            else ("text-red-400 font-bold" if st_l in ("rejected", "failed", "broken") else "text-amber-400")
                                        )
                                        with ui.row().classes("w-full justify-between items-center"):
                                            ui.label(b_name).classes("text-xs agy-mono text-slate-200")
                                            ui.label(b_st.upper()).classes(f"text-xs agy-mono {color}")

                            # Секція активних коментарів та рев'юверів
                            active_threads = snap.get("active_threads", [])
                            reviewers = {k: v for k, v in snap.get("reviewers", {}).items() if v != 0}

                            with ui.row().classes("w-full gap-3 no-wrap"):
                                with ui.column().classes("w-1/2 bg-[#0b0d12] p-3 rounded-lg border border-[#1e2433] gap-1"):
                                    ui.label(f"Активні коментарі ({len(active_threads)})").classes(
                                        "text-xs font-semibold text-slate-400"
                                    )
                                    if not active_threads:
                                        ui.label("Усі треди вирішені ✅").classes("text-xs text-emerald-400")
                                    else:
                                        for th in active_threads[:5]:
                                            lc = th["last_comment"]
                                            author = lc.get("author", {}).get("displayName", "Unknown")
                                            snippet = lc.get("content", "").replace("\n", " ")[:70]
                                            ui.label(f"💬 {author}: {snippet}").classes("text-xs text-amber-300")

                                with ui.column().classes("w-1/2 bg-[#0b0d12] p-3 rounded-lg border border-[#1e2433] gap-1"):
                                    ui.label("Голоси рев'юверів").classes("text-xs font-semibold text-slate-400")
                                    if not reviewers:
                                        ui.label("Ще немає голосів").classes("text-xs text-slate-500")
                                    else:
                                        for r_name, vote in reviewers.items():
                                            v_txt = (
                                                "Approved (+10)"
                                                if vote == 10
                                                else ("Suggestions (+5)" if vote == 5 else ("Waiting (-5)" if vote == -5 else f"Rejected ({vote})"))
                                            )
                                            v_col = "text-emerald-400" if vote > 0 else "text-red-400"
                                            with ui.row().classes("w-full justify-between"):
                                                ui.label(r_name).classes("text-xs text-slate-200")
                                                ui.label(v_txt).classes(f"text-xs agy-mono {v_col}")

            # Права частина: журнал подій
            with ui.card().classes("w-4/12 agy-card p-5 gap-3"):
                ui.label("Журнал подій (Live Feed)").classes("text-base font-semibold text-white")
                if not state.pr_events:
                    ui.label("Поки що змін у статусах PR не зафіксовано.").classes("text-xs text-slate-500")
                else:
                    with ui.column().classes("w-full gap-2 max-h-[60vh] overflow-y-auto"):
                        for ev in state.pr_events:
                            with ui.row().classes("w-full bg-[#0b0d12] p-2.5 rounded border border-[#1e2433] gap-2"):
                                ui.label(ev["time"]).classes("text-[11px] agy-mono text-slate-500 shrink-0")
                                ui.label(ev["text"]).classes("text-xs text-slate-200")

    pr_cards_view()


# ================= 5. СТОРІНКА: LINKS & SETTINGS =================

def render_links_and_settings_page():
    with ui.tabs().classes("w-full border-b border-[#1e2433] mb-4") as subtabs:
        tab_links = ui.tab("Швидкі лінки (Bookmarks)", icon="bookmarks")
        tab_cfg = ui.tab("Налаштування (settings.json)", icon="settings")

    with ui.tab_panels(subtabs, value=tab_links).classes("w-full bg-transparent"):
        # ---- Вкладка Швидких Лінків ----
        with ui.tab_panel(tab_links).classes("p-0"):
            @ui.refreshable
            def links_view():
                links = load_links()

                with ui.row().classes("w-full justify-between items-center mb-4"):
                    ui.label("Швидкі ресурси та закладки (data/links.json)").classes("text-xl font-bold text-white")
                    ui.button("Додати лінк", icon="add", on_click=lambda: add_dialog.open()).props(
                        "unelevated color=blue-6 no-caps"
                    )

                with ui.dialog() as add_dialog, ui.card().classes("agy-card p-6 w-96 gap-3"):
                    ui.label("Новий ресурс").classes("text-lg font-bold text-white")
                    t_in = ui.input("Назва").props("outlined dark dense").classes("w-full")
                    c_in = ui.input("Категорія", value="Azure DevOps").props("outlined dark dense").classes("w-full")
                    u_in = ui.input("URL (https://...)").props("outlined dark dense").classes("w-full")
                    d_in = ui.input("Короткий опис").props("outlined dark dense").classes("w-full")

                    def save_new_link():
                        if not t_in.value or not u_in.value:
                            ui.notify("Заповніть назву та URL", type="warning")
                            return
                        links.append({
                            "title": t_in.value.strip(),
                            "category": (c_in.value or "Інше").strip(),
                            "url": u_in.value.strip(),
                            "icon": "link",
                            "description": (d_in.value or "").strip(),
                        })
                        save_links(links)
                        add_dialog.close()
                        links_view.refresh()

                    with ui.row().classes("w-full justify-end gap-2 mt-2"):
                        ui.button("Скасувати", on_click=add_dialog.close).props("flat color=grey-5 no-caps")
                        ui.button("Зберегти", on_click=save_new_link).props("unelevated color=blue-6 no-caps")

                def delete_link(idx: int):
                    if 0 <= idx < len(links):
                        links.pop(idx)
                        save_links(links)
                        links_view.refresh()

                with ui.row().classes("w-full gap-4 flex-wrap"):
                    for idx, item in enumerate(links):
                        with ui.card().classes("w-80 agy-card agy-card-hover p-4 justify-between"):
                            with ui.column().classes("w-full gap-1.5"):
                                with ui.row().classes("w-full justify-between items-center"):
                                    ui.badge(item.get("category", "Link"), color="blue-9").classes("text-[11px]")
                                    ui.button(
                                        icon="delete_outline",
                                        on_click=lambda _, i=idx: delete_link(i),
                                    ).props("flat round dense size=xs color=grey-6")
                                ui.label(item.get("title", "Без назви")).classes(
                                    "text-base font-semibold text-white mt-1"
                                )
                                ui.label(item.get("description", "")).classes("text-xs text-slate-400")
                            ui.button(
                                "Відкрити",
                                icon="open_in_new",
                                on_click=lambda _, u=item.get("url", ""): ui.navigate.to(u, new_tab=True),
                            ).props("outline dense color=blue-4 no-caps").classes("w-full mt-3")

            links_view()

        # ---- Вкладка Налаштувань ----
        with ui.tab_panel(tab_cfg).classes("p-0"):
            cfg = load_settings()
            with ui.card().classes("w-full max-w-3xl agy-card p-6 gap-4"):
                ui.label("Локальна конфігурація (data/settings.json)").classes("text-lg font-bold text-white")
                ui.label(
                    "Цей файл додано в .gitignore, тому твої корпоративні назви та шляхи ніколи не потраплять у публічний GitHub."
                ).classes("text-xs text-emerald-400")

                with ui.row().classes("w-full gap-4"):
                    org_in = ui.input("Organization (ADO)", value=cfg.get("organization", "")).props("outlined dark dense").classes("flex-1")
                    proj_in = ui.input("Project (ADO)", value=cfg.get("project", "")).props("outlined dark dense").classes("flex-1")
                    repo_in = ui.input("Repository ID", value=cfg.get("repository_id", "")).props("outlined dark dense").classes("flex-1")

                with ui.row().classes("w-full gap-4"):
                    query_in = ui.input("Sprint Wiql Query ID", value=cfg.get("query_id", "")).props("outlined dark dense").classes("flex-1 agy-mono")
                    target_in = ui.input("Target Branch (PR)", value=cfg.get("target_branch", "master")).props("outlined dark dense").classes("w-44 agy-mono")

                with ui.row().classes("w-full gap-4"):
                    bpref_in = ui.input("Branch Prefix", value=cfg.get("branch_prefix", "")).props("outlined dark dense").classes("flex-1 agy-mono")
                    upref_in = ui.input("User Prefix", value=cfg.get("user_prefix", "")).props("outlined dark dense").classes("flex-1 agy-mono")
                    inter_in = ui.number("PR Watch Interval (сек)", value=cfg.get("pr_watch_interval", 60), min=10, max=600).props("outlined dark dense").classes("w-48")

                roots_in = (
                    ui.input(
                        "Git Roots (через кому)",
                        value=", ".join(cfg.get("git_roots", [])),
                    )
                    .props("outlined dark dense")
                    .classes("w-full agy-mono")
                )
                clang_in = (
                    ui.input("Шлях до clang-format.exe", value=cfg.get("clang_format_path", ""))
                    .props("outlined dark dense")
                    .classes("w-full agy-mono")
                )

                def save_cfg():
                    new_cfg = {
                        "organization": org_in.value.strip(),
                        "project": proj_in.value.strip(),
                        "repository_id": repo_in.value.strip(),
                        "target_branch": target_in.value.strip(),
                        "query_id": query_in.value.strip(),
                        "branch_prefix": bpref_in.value.strip(),
                        "user_prefix": upref_in.value.strip(),
                        "git_roots": [r.strip() for r in roots_in.value.split(",") if r.strip()],
                        "clang_format_path": clang_in.value.strip(),
                        "pr_watch_interval": int(inter_in.value or 60),
                        "pr_watch_notify": state.pr_notify,
                        "pr_watch_sound": state.pr_sound,
                        "pr_watch_include_drafts": state.pr_include_drafts,
                    }
                    save_settings(new_cfg)
                    state.pr_interval = new_cfg["pr_watch_interval"]
                    ui.notify("Налаштування збережено у data/settings.json!", type="positive")

                with ui.row().classes("w-full justify-end mt-2"):
                    ui.button("Зберегти налаштування", icon="save", on_click=save_cfg).props(
                        "unelevated color=blue-6 no-caps"
                    )


if __name__ in {"__main__", "__mp_main__"}:
    ui.run(
        title="Antigravity Automation Hub",
        host="127.0.0.1",
        port=8420,
        dark=True,
        reload=False,
        show=True,
    )
