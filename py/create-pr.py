import json
import os
import sys
import re
import subprocess
from pathlib import Path
import requests
from requests.auth import HTTPBasicAuth


def _load_local_settings() -> dict:
    """Шукає data/settings.json поруч зі скриптом або в домашній папці Automation."""
    candidates = [
        Path(__file__).resolve().parent / "data" / "settings.json",
        Path.home() / "Automation" / "data" / "settings.json",
    ]
    for path in candidates:
        if path.exists():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except Exception:
                pass
    return {}


# Конфігурація Azure DevOps
_CFG = _load_local_settings()
ORGANIZATION = _CFG.get("organization") or os.environ.get("ADO_ORG", "your-org")
PROJECT = _CFG.get("project") or os.environ.get("ADO_PROJECT", "YourProject")
REPOSITORY_ID = _CFG.get("repository_id") or os.environ.get("ADO_REPO", PROJECT)   # Назва або GUID репозиторію
TARGET_BRANCH = _CFG.get("target_branch", "master")   # Гілка, в яку вливаємо PR

PAT = os.environ.get("ADO_PAT")
if not PAT:
    print("Помилка: змінна середовища ADO_PAT не знайдена.", file=sys.stderr)
    sys.exit(1)

auth = HTTPBasicAuth("", PAT)
headers = {"Content-Type": "application/json"}


def get_current_branch(repo_dir: Path) -> str:
    """Визначає поточну Git-гілку в локальній репозиторії."""
    res = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=str(repo_dir),
        capture_output=True,
        text=True
    )
    if res.returncode != 0:
        print("Помилка отримання поточної гілки git.", file=sys.stderr)
        sys.exit(1)
    return res.stdout.strip()


def get_last_commit_subject(repo_dir: Path) -> str:
    """Отримує перший рядок (subject) останнього коміту."""
    res = subprocess.run(
        ["git", "log", "-1", "--pretty=%s"],
        cwd=str(repo_dir),
        capture_output=True,
        text=True
    )
    if res.returncode == 0 and res.stdout.strip():
        return res.stdout.strip()
    return ""


def parse_task_md(current_dir: Path) -> dict:
    """Знаходить xxxxx_*.md і парсить task_id та title."""
    md_files = list(current_dir.glob("*_*.md"))
    task_files = [f for f in md_files if re.match(r"^\d+_", f.name)]

    if not task_files:
        return {"id": None, "title": "", "desc": ""}

    file_path = task_files[0]
    content = file_path.read_text(encoding="utf-8")

    task_id = None
    title = ""
    match = re.search(r"^#\s*\[(\w+)\s*#(\d+)\]\s*(.*)$", content, re.MULTILINE)
    if match:
        task_id = int(match.group(2))
        title = f"[{match.group(1)} #{task_id}] {match.group(3).strip()}"

    return {"id": task_id, "title": title, "desc": f"Fix/Work for Work Item #{task_id}"}


def main():
    current_dir = Path.cwd()
    repo_dir = current_dir / REPOSITORY_ID

    if not repo_dir.exists() or not (repo_dir / ".git").exists():
        print(f"Помилка: підпапку Git-репозиторію не знайдено за шляхом {repo_dir}", file=sys.stderr)
        sys.exit(1)

    # 1. Визначаємо поточну гілку
    branch = get_current_branch(repo_dir)
    if branch in ("master", "main", "HEAD"):
        print(f"Помилка: ви знаходитесь на гілці '{branch}'. Перемкніться на робочу гілку задачі.", file=sys.stderr)
        sys.exit(1)

    print(f"Поточна гілка: {branch}")

    # 2. Отримуємо дефолтний заголовок з останнього коміту
    commit_subject = get_last_commit_subject(repo_dir)
    task_info = parse_task_md(current_dir)

    default_title = commit_subject or task_info["title"] or f"Fix for {branch}"
    
    # Запит у користувача
    print(f"\nЗапропонований Title для PR:\n  [{default_title}]")
    custom_title = input("Натисніть Enter, щоб підтвердити, або введіть новий Title: ").strip()
    pr_title = custom_title if custom_title else default_title

    # 3. Перевіряємо та пушимо гілку на віддалений сервер
    print("\nПуш змін на origin...")
    push_res = subprocess.run(
        ["git", "push", "-u", "origin", branch],
        cwd=str(repo_dir)
    )
    if push_res.returncode != 0:
        print("Помилка при спробі зробити git push.", file=sys.stderr)
        sys.exit(1)

    # 4. Створюємо Pull Request через API
    pr_desc = task_info["desc"] or f"Changes from branch {branch}"
    url = f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/git/repositories/{REPOSITORY_ID}/pullrequests?api-version=7.0"

    payload = {
        "sourceRefName": f"refs/heads/{branch}",
        "targetRefName": f"refs/heads/{TARGET_BRANCH}",
        "title": pr_title,
        "description": pr_desc,
    }

    # Підв'язуємо Work Item до PR, якщо знайдено ID
    if task_info["id"]:
        payload["workItemRefs"] = [{
            "id": str(task_info["id"]),
            "url": f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/wit/workItems/{task_info['id']}"
        }]

    print(f"\nСтворення Pull Request: '{pr_title}' -> '{TARGET_BRANCH}'...")
    res = requests.post(url, json=payload, auth=auth, headers=headers)

    if res.status_code == 409:
        print("\n[!] PR для цієї гілки вже існує. Отримання існуючого PR...")
        get_url = f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/git/repositories/{REPOSITORY_ID}/pullrequests?searchCriteria.sourceRefName=refs/heads/{branch}&api-version=7.0"
        exist_res = requests.get(get_url, auth=auth, headers=headers).json()
        if exist_res.get("value"):
            pr_data = exist_res["value"][0]
            pr_id = pr_data["pullRequestId"]
            print(f"Існуючий PR #{pr_id}: https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_git/{REPOSITORY_ID}/pullrequest/{pr_id}")
            return

    res.raise_for_status()
    pr_data = res.json()
    pr_id = pr_data["pullRequestId"]
    pr_web_url = f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_git/{REPOSITORY_ID}/pullrequest/{pr_id}"

    print("\n" + "=" * 80)
    print("Pull Request успішно створено!")
    print(f"PR ID:  #{pr_id}")
    print(f"Title:  {pr_title}")
    print(f"URL:    {pr_web_url}")
    print("=" * 80)


if __name__ == "__main__":
    main()
