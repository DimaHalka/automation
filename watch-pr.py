#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
watch-pr.py - Моніторинг активних Pull Request у Azure DevOps.

Відстежує:
  1. Статус валідаційних (Gated) білдів (running, queued, approved, rejected/failed).
  2. Нові коментарі від рев'юверів та активні обговорення.
  3. Зміни голосів рев'юверів (Approved, Rejected, Waiting).

Запуск:
  python watch-pr.py                # Запуск циклічного моніторингу (кожні 60 с)
  python watch-pr.py --interval 30  # З інтервалом 30 секунд
  python watch-pr.py --once         # Одноразова перевірка та вихід
  python watch-pr.py --no-notify    # Без спливаючих сповіщень Windows
"""

import argparse
import base64
import os
import subprocess
import sys
import time
from datetime import datetime
from typing import Any, Dict, List, Optional, Tuple

import requests
from requests.auth import HTTPBasicAuth

# Налаштування кодування для коректного виводу в Windows консоль
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

# Спробуємо підключити colorama для кольорів у консолі
try:
    from colorama import Fore, Style, init
    init(autoreset=True)
    HAS_COLORAMA = True
except ImportError:
    HAS_COLORAMA = False
    class _EmptyColor:
        def __getattr__(self, _):
            return ""
    Fore = _EmptyColor()
    Style = _EmptyColor()

# Спробуємо підключити winsound для звукового сигналу на Windows
try:
    import winsound
    HAS_WINSOUND = True
except ImportError:
    HAS_WINSOUND = False

from config import load_settings

# ================= КОНФІГУРАЦІЯ AZURE DEVOPS =================
_CFG = load_settings()
ORGANIZATION = _CFG["organization"]
PROJECT = _CFG["project"]
REPOSITORY_ID = _CFG["repository_id"]

PAT = os.environ.get("ADO_PAT", "")
AUTH = HTTPBasicAuth("", PAT)
HEADERS = {"Content-Type": "application/json"}


# ================= СИСТЕМНІ СПОВІЩЕННЯ WINDOWS =================
def send_toast_notification(title: str, message: str) -> None:
    """Надсилає спливаюче системне сповіщення Windows (Toast)."""
    if sys.platform != "win32":
        return
    try:
        # Безпечне екранування символів для XML
        safe_title = title.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', '&quot;')
        safe_msg = message.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', '&quot;')

        ps_code = f"""
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
$template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)
$textNodes = $template.GetElementsByTagName("text")
$textNodes.Item(0).AppendChild($template.CreateTextNode("{safe_title}")) | Out-Null
$textNodes.Item(1).AppendChild($template.CreateTextNode("{safe_msg}")) | Out-Null
$toast = [Windows.UI.Notifications.ToastNotification]::new($template)
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("Azure DevOps PR Monitor").Show($toast)
"""
        encoded = base64.b64encode(ps_code.encode("utf-16le")).decode("ascii")
        creation_flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0
        subprocess.Popen(
            ["powershell", "-NoProfile", "-EncodedCommand", encoded],
            creationflags=creation_flags,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
    except Exception:
        pass


def play_alert_sound(is_error: bool = False) -> None:
    """Відтворює звуковий сигнал під час важливої події."""
    if HAS_WINSOUND and sys.platform == "win32":
        try:
            if is_error:
                winsound.MessageBeep(winsound.MB_ICONHAND)
            else:
                winsound.MessageBeep(winsound.MB_ICONASTERISK)
        except Exception:
            pass


# ================= РОБОТА З AZURE DEVOPS API =================
def get_current_user_id() -> str:
    """Визначає ID поточного користувача через connectionData."""
    url = f"https://dev.azure.com/{ORGANIZATION}/_apis/connectionData"
    res = requests.get(url, auth=AUTH, headers=HEADERS, timeout=15)
    res.raise_for_status()
    user_data = res.json().get("authenticatedUser", {})
    user_id = user_data.get("id")
    if not user_id:
        raise RuntimeError("Не вдалося отримати ID автентифікованого користувача.")
    return user_id


def get_active_pull_requests(user_id: str, include_drafts: bool = False) -> List[Dict[str, Any]]:
    """Отримує список активних PR поточного користувача (ігноруючи Draft за замовчуванням)."""
    url = (
        f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/git/repositories/{REPOSITORY_ID}"
        f"/pullrequests?searchCriteria.creatorId={user_id}&searchCriteria.status=active&api-version=7.1"
    )
    res = requests.get(url, auth=AUTH, headers=HEADERS, timeout=20)
    res.raise_for_status()
    prs = res.json().get("value", [])
    if not include_drafts:
        prs = [pr for pr in prs if not pr.get("isDraft", False)]
    return prs


def get_pr_policy_evaluations(project_id: str, pr_id: int) -> List[Dict[str, Any]]:
    """Отримує оцінки політик (включаючи білди) для конкретного PR."""
    url = (
        f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/policy/evaluations"
        f"?artifactId=vstfs:///CodeReview/CodeReviewId/{project_id}/{pr_id}&api-version=7.1-preview.1"
    )
    res = requests.get(url, auth=AUTH, headers=HEADERS, timeout=20)
    if not res.ok:
        return []
    return res.json().get("value", [])


def get_pr_threads(pr_id: int) -> List[Dict[str, Any]]:
    """Отримує всі обговорення (threads) конкретного PR."""
    url = (
        f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/git/repositories/{REPOSITORY_ID}"
        f"/pullrequests/{pr_id}/threads?api-version=7.1"
    )
    res = requests.get(url, auth=AUTH, headers=HEADERS, timeout=20)
    if not res.ok:
        return []
    return res.json().get("value", [])


# ================= АНАЛІЗ ТА ФОРМАТУВАННЯ СТАНУ =================
def format_build_status(status: str) -> str:
    """Повертає форматований статус білда з піктограмою та кольором."""
    st = (status or "").lower()
    if st in ("approved", "succeeded"):
        return f"{Fore.GREEN}🟢 {status}{Style.RESET_ALL}"
    elif st in ("running",):
        return f"{Fore.YELLOW}🟡 {status}{Style.RESET_ALL}"
    elif st in ("queued",):
        return f"{Fore.CYAN}⏳ {status}{Style.RESET_ALL}"
    elif st in ("rejected", "failed", "broken"):
        return f"{Fore.RED}{Style.BRIGHT}🔴 {status}{Style.RESET_ALL}"
    return f"{Fore.WHITE}{status}{Style.RESET_ALL}"


def format_vote(vote: int) -> str:
    """Форматує голос рев'ювера."""
    if vote == 10:
        return f"{Fore.GREEN}Approved (+10){Style.RESET_ALL}"
    elif vote == 5:
        return f"{Fore.YELLOW}Approved with suggestions (+5){Style.RESET_ALL}"
    elif vote == -5:
        return f"{Fore.MAGENTA}Waiting for author (-5){Style.RESET_ALL}"
    elif vote == -10:
        return f"{Fore.RED}Rejected (-10){Style.RESET_ALL}"
    return f"{Fore.WHITE}No vote (0){Style.RESET_ALL}"


def parse_pr_snapshot(pr: Dict[str, Any]) -> Dict[str, Any]:
    """Збирає поточний повний зліпок стану PR (білди, коментарі, рев'ювери)."""
    pr_id = pr["pullRequestId"]
    title = pr.get("title", "")
    source_branch = pr.get("sourceRefName", "").replace("refs/heads/", "")
    project_id = pr.get("repository", {}).get("project", {}).get("id", "")
    web_url = f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_git/{REPOSITORY_ID}/pullrequest/{pr_id}"

    # 1. Gated Builds
    raw_evals = get_pr_policy_evaluations(project_id, pr_id)
    builds = {}
    for ev in raw_evals:
        conf = ev.get("configuration", {})
        c_type = conf.get("type", {}).get("displayName", "")
        settings = conf.get("settings", {})
        display_name = settings.get("displayName") or c_type or "Build"
        st = ev.get("status", "unknown")
        if c_type == "Build":
            builds[display_name] = st

    # 2. Threads / Comments
    raw_threads = get_pr_threads(pr_id)
    comments_map = {}   # (thread_id, comment_id) -> comment_dict
    active_threads = []
    
    for th in raw_threads:
        if th.get("isDeleted"):
            continue
        th_id = th.get("id")
        th_status = th.get("status")  # active, fixed, wontFix, closed, byDesign
        raw_comments = th.get("comments", [])
        
        # Відфільтровуємо системні сповіщення ADO
        user_comments = [c for c in raw_comments if c.get("commentType") != "system" and not c.get("isDeleted")]
        if not user_comments:
            continue

        for c in user_comments:
            c_id = c.get("id")
            comments_map[(th_id, c_id)] = {
                "thread_id": th_id,
                "comment_id": c_id,
                "author": c.get("author", {}).get("displayName", "Unknown"),
                "date": c.get("publishedDate", ""),
                "content": c.get("content", "").strip()
            }

        # Чи є тред активним (не закритим)
        if th_status in ("active", "pending") or th_status is None:
            active_threads.append({
                "thread_id": th_id,
                "status": th_status or "active",
                "last_comment": user_comments[-1]
            })

    # 3. Reviewers & Votes
    reviewers = {}
    for r in pr.get("reviewers", []):
        r_name = r.get("displayName", "Unknown")
        vote = r.get("vote", 0)
        reviewers[r_name] = vote

    return {
        "id": pr_id,
        "title": title,
        "branch": source_branch,
        "url": web_url,
        "builds": builds,
        "comments_map": comments_map,
        "active_threads": active_threads,
        "reviewers": reviewers,
    }


def print_pr_card(snap: Dict[str, Any]) -> None:
    """Виводить детальну картку стану PR у консоль."""
    pr_id = snap["id"]
    title = snap["title"]
    branch = snap["branch"]
    url = snap["url"]
    builds = snap["builds"]
    active_threads = snap["active_threads"]
    total_comments = len(snap["comments_map"])
    reviewers = snap["reviewers"]

    print(f"\n{Fore.CYAN}┌{'─' * 76}┐{Style.RESET_ALL}")
    print(f"{Fore.CYAN}│{Style.RESET_ALL} {Fore.WHITE}{Style.BRIGHT}PR #{pr_id}:{Style.RESET_ALL} {title[:66]}")
    print(f"{Fore.CYAN}│{Style.RESET_ALL}   {Fore.BLUE}Гілка:{Style.RESET_ALL} {branch}")
    print(f"{Fore.CYAN}│{Style.RESET_ALL}   {Fore.BLUE}URL:{Style.RESET_ALL}   {url}")

    # Вивід статусів білдів
    if builds:
        failed_count = sum(1 for st in builds.values() if st in ("rejected", "failed", "broken"))
        running_count = sum(1 for st in builds.values() if st in ("running",))
        approved_count = sum(1 for st in builds.values() if st in ("approved", "succeeded"))
        total = len(builds)

        summary_color = Fore.RED if failed_count > 0 else (Fore.YELLOW if running_count > 0 else Fore.GREEN)
        print(f"{Fore.CYAN}│{Style.RESET_ALL}")
        print(f"{Fore.CYAN}│{Style.RESET_ALL}   {summary_color}{Style.BRIGHT}Gated Builds ({approved_count}/{total} пройшли, {running_count} в процесі, {failed_count} впало):{Style.RESET_ALL}")

        for b_name, b_status in sorted(builds.items()):
            formatted_st = format_build_status(b_status)
            print(f"{Fore.CYAN}│{Style.RESET_ALL}     • {b_name:<36} {formatted_st}")
    else:
        print(f"{Fore.CYAN}│{Style.RESET_ALL}   {Fore.WHITE}Gated Builds: немає налаштованих білдів.{Style.RESET_ALL}")

    # Вивід коментарів
    print(f"{Fore.CYAN}│{Style.RESET_ALL}")
    if active_threads:
        print(f"{Fore.CYAN}│{Style.RESET_ALL}   {Fore.MAGENTA}{Style.BRIGHT}Коментарі ({len(active_threads)} активних / {total_comments} всього):{Style.RESET_ALL}")
        for th in active_threads:
            last_c = th["last_comment"]
            author = last_c.get("author", {}).get("displayName", "Unknown")
            snippet = last_c.get("content", "").replace("\n", " ")[:60]
            print(f"{Fore.CYAN}│{Style.RESET_ALL}     💬 {Fore.YELLOW}{author}:{Style.RESET_ALL} \"{snippet}...\"")
    else:
        comment_label = f"Всі вирішені ({total_comments} всього)" if total_comments else "Коментарів немає"
        print(f"{Fore.CYAN}│{Style.RESET_ALL}   {Fore.GREEN}Коментарі:{Style.RESET_ALL} 0 активних ({comment_label})")

    # Вивід рев'юверів
    voted_reviewers = {k: v for k, v in reviewers.items() if v != 0}
    if voted_reviewers:
        print(f"{Fore.CYAN}│{Style.RESET_ALL}")
        print(f"{Fore.CYAN}│{Style.RESET_ALL}   {Fore.WHITE}{Style.BRIGHT}Рев'ювери з голосами:{Style.RESET_ALL}")
        for name, vote in voted_reviewers.items():
            print(f"{Fore.CYAN}│{Style.RESET_ALL}     👤 {name:<30} {format_vote(vote)}")

    print(f"{Fore.CYAN}└{'─' * 76}┘{Style.RESET_ALL}")


# ================= ПОРІВНЯННЯ ТА ДЕТЕКЦІЯ ЗМІН =================
def compare_snapshots(
    old_snaps: Dict[int, Dict[str, Any]],
    new_snaps: Dict[int, Dict[str, Any]],
    notify_enabled: bool,
    sound_enabled: bool
) -> List[str]:
    """
    Порівнює два стани активних PR.
    Повертає список виявлених змін та надсилає сповіщення.
    """
    events = []

    # 1. Перевірка нових та закритих PR
    for pr_id, new_pr in new_snaps.items():
        if pr_id not in old_snaps:
            msg = f"Новий активний PR #{pr_id}: {new_pr['title']}"
            events.append(f"{Fore.GREEN}✨ [PR #{pr_id}] {msg}{Style.RESET_ALL}")
            if notify_enabled:
                send_toast_notification(f"Новий PR #{pr_id}", new_pr['title'])
            if sound_enabled:
                play_alert_sound(is_error=False)

    for pr_id, old_pr in old_snaps.items():
        if pr_id not in new_snaps:
            msg = f"PR #{pr_id} більше не активний (завершено або скасовано)"
            events.append(f"{Fore.MAGENTA}🏁 [PR #{pr_id}] {msg}{Style.RESET_ALL}")
            if notify_enabled:
                send_toast_notification(f"PR #{pr_id} закрито", old_pr['title'])

    # 2. Зміни всередині існуючих PR
    for pr_id, new_pr in new_snaps.items():
        if pr_id not in old_snaps:
            continue
        old_pr = old_snaps[pr_id]

        # А) Зміни у статусах білдів
        for b_name, new_st in new_pr["builds"].items():
            old_st = old_pr["builds"].get(b_name)
            if old_st != new_st:
                if new_st in ("rejected", "failed", "broken"):
                    events.append(f"{Fore.RED}{Style.BRIGHT}🚨 [PR #{pr_id}] Білд '{b_name}' ВПАВ ({new_st})!{Style.RESET_ALL}")
                    if notify_enabled:
                        send_toast_notification(f"Білд впав! (PR #{pr_id})", f"{b_name}: {new_st}")
                    if sound_enabled:
                        play_alert_sound(is_error=True)
                elif new_st in ("approved", "succeeded"):
                    events.append(f"{Fore.GREEN}✅ [PR #{pr_id}] Білд '{b_name}' успішно пройдено!{Style.RESET_ALL}")
                    if notify_enabled:
                        send_toast_notification(f"Білд пройдено (PR #{pr_id})", f"{b_name}: {new_st}")
                    if sound_enabled:
                        play_alert_sound(is_error=False)
                elif new_st == "running" and old_st != "running":
                    events.append(f"{Fore.YELLOW}⚙️ [PR #{pr_id}] Білд '{b_name}' запущено ({new_st}).{Style.RESET_ALL}")

        # Б) Нові коментарі
        old_comments = old_pr["comments_map"]
        new_comments = new_pr["comments_map"]
        for key, c_info in new_comments.items():
            if key not in old_comments:
                author = c_info["author"]
                snippet = c_info["content"][:80].replace("\n", " ")
                events.append(f"{Fore.YELLOW}{Style.BRIGHT}💬 [PR #{pr_id}] Новий коментар від {author}: \"{snippet}...\"{Style.RESET_ALL}")
                if notify_enabled:
                    send_toast_notification(f"Новий коментар у PR #{pr_id}", f"{author}: {snippet}")
                if sound_enabled:
                    play_alert_sound(is_error=False)

        # В) Зміни голосів рев'юверів
        for r_name, new_vote in new_pr["reviewers"].items():
            old_vote = old_pr["reviewers"].get(r_name, 0)
            if new_vote != old_vote:
                vote_str = "Approved" if new_vote == 10 else ("Rejected" if new_vote == -10 else f"Vote: {new_vote}")
                events.append(f"{Fore.CYAN}🗳️ [PR #{pr_id}] Рев'ювер {r_name} змінив голос: {vote_str}{Style.RESET_ALL}")
                if notify_enabled:
                    send_toast_notification(f"Голос у PR #{pr_id}", f"{r_name}: {vote_str}")
                if sound_enabled:
                    play_alert_sound(is_error=(new_vote == -10))

    return events


# ================= ГОЛОВНИЙ ЦИКЛ =================
def run_monitor(interval: int, once: bool, notify: bool, sound: bool, include_drafts: bool = False) -> None:
    """Головний цикл відстеження PR."""
    if not PAT:
        print(f"{Fore.RED}Помилка: змінна середовища ADO_PAT не знайдена!{Style.RESET_ALL}", file=sys.stderr)
        print("Встановіть її перед запуском: $env:ADO_PAT=\"ваш_токен\"", file=sys.stderr)
        sys.exit(1)
    print(f"{Fore.CYAN}{Style.BRIGHT}=== Azure DevOps PR Watcher ==={Style.RESET_ALL}")
    print(f"Репозиторій: {ORGANIZATION}/{PROJECT}/{REPOSITORY_ID}")
    draft_status = "включено" if include_drafts else "ігноруються"
    print(f"Інтервал: {interval} с | Сповіщення: {'увімкнено' if notify else 'вимкнено'} | Draft PR: {draft_status}")
    print(f"Для виходу натисніть {Fore.YELLOW}Ctrl+C{Style.RESET_ALL}\n")

    try:
        user_id = get_current_user_id()
    except Exception as e:
        print(f"{Fore.RED}Помилка автентифікації в Azure DevOps: {e}{Style.RESET_ALL}", file=sys.stderr)
        sys.exit(1)

    previous_snaps: Optional[Dict[int, Dict[str, Any]]] = None

    while True:
        now_str = datetime.now().strftime("%H:%M:%S")

        try:
            raw_prs = get_active_pull_requests(user_id, include_drafts=include_drafts)
            current_snaps = {}
            for pr in raw_prs:
                current_snaps[pr["pullRequestId"]] = parse_pr_snapshot(pr)

            # Перший запуск: виводимо повні картки всіх PR
            if previous_snaps is None:
                print(f"[{now_str}] Знайдено активних PR: {len(current_snaps)}")
                if not current_snaps:
                    print(f"{Fore.GREEN}У вас немає активних Pull Request.{Style.RESET_ALL}")
                else:
                    for snap in current_snaps.values():
                        print_pr_card(snap)
                previous_snaps = current_snaps

            else:
                # Порівнюємо стан з попереднім
                events = compare_snapshots(previous_snaps, current_snaps, notify, sound)
                if events:
                    print(f"\n[{now_str}] {Fore.YELLOW}{Style.BRIGHT}--- Виявлено оновлення! ---{Style.RESET_ALL}")
                    for ev in events:
                        print(f"  {ev}")
                    # Оновлюємо картки PR, де сталися зміни
                    for pr_id, snap in current_snaps.items():
                        if pr_id not in previous_snaps or previous_snaps[pr_id] != snap:
                            print_pr_card(snap)
                    previous_snaps = current_snaps
                else:
                    # Пульс без спаму
                    pr_count = len(current_snaps)
                    print(f"\r[{now_str}] Статус без змін ({pr_count} активних PR). Очікування {interval} с...  ", end="", flush=True)

        except requests.exceptions.RequestException as e:
            print(f"\n[{now_str}] {Fore.RED}Помилка зв'язку з ADO: {e}. Повтор через {interval} с...{Style.RESET_ALL}")
        except Exception as e:
            print(f"\n[{now_str}] {Fore.RED}Помилка обробки даних: {e}{Style.RESET_ALL}")

        if once:
            print(f"\n[{now_str}] Режим '--once' завершено.")
            break

        try:
            time.sleep(interval)
        except KeyboardInterrupt:
            print(f"\n\n{Fore.YELLOW}👋 Моніторинг зупинено користувачем.{Style.RESET_ALL}")
            break


def main():
    parser = argparse.ArgumentParser(
        description="Моніторинг статусів активних Pull Request, білдів та коментарів в Azure DevOps."
    )
    parser.add_argument(
        "-i", "--interval",
        type=int,
        default=60,
        help="Інтервал опитування сервера у секундах (за замовчуванням: 60)"
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="Виконати одну перевірку, вивести стан і вийти"
    )
    parser.add_argument(
        "--no-notify",
        action="store_true",
        help="Вимкнути спливаючі системні сповіщення Windows (Toast notifications)"
    )
    parser.add_argument(
        "--no-sound",
        action="store_true",
        help="Вимкнути звукові сигнали"
    )
    parser.add_argument(
        "--include-drafts",
        action="store_true",
        help="Включати чернетки (Draft PR) у моніторинг (за замовчуванням вимкнено)"
    )

    args = parser.parse_args()

    run_monitor(
        interval=max(5, args.interval),
        once=args.once,
        notify=not args.no_notify,
        sound=not args.no_sound,
        include_drafts=args.include_drafts
    )


if __name__ == "__main__":
    main()
