import os
import sys
import re
import html
import shutil
from pathlib import Path
from collections import Counter
import requests
from requests.auth import HTTPBasicAuth
from config import get_git_roots, load_settings

# Конфігурація
_CFG = load_settings()
ORGANIZATION = _CFG["organization"]
PROJECT = _CFG["project"]
QUERY_ID = _CFG["query_id"]
OUTPUT_DIR = "sprint-tasks"

GIT_ROOTS = get_git_roots(_CFG)

PAT = os.environ.get("ADO_PAT")
if not PAT:
    print("Помилка: змінна середовища ADO_PAT не знайдена.", file=sys.stderr)
    sys.exit(1)

auth = HTTPBasicAuth("", PAT)
headers = {"Content-Type": "application/json"}

def clean_html(raw_html: str) -> str:
    """Очищає HTML-розмітку ADO в чистий читабельний Markdown/текст."""
    if not raw_html:
        return ""
    text = re.sub(r"<br\s*/?>", "\n", raw_html)
    text = re.sub(r"</p>", "\n\n", raw_html)
    text = re.sub(r"<[^>]+>", "", text)
    return html.unescape(text).strip()

def clear_output_dir(path: str):
    """Повністю очищає вміст каталогу, створюючи його наново."""
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(path, exist_ok=True)

def build_task_to_folder_map() -> dict[int, Path]:
    """Сканує робочі каталоги та знаходить відповідність між task_id і папкою."""
    task_folders = {}
    for root in GIT_ROOTS:
        if not root.exists():
            continue
        for entry in root.iterdir():
            if not entry.is_dir() or entry.name.startswith("."):
                continue
            
            # Шукаємо md-файли задач у корені кожної робочої теки
            for md_file in entry.glob("*_*.md"):
                match = re.match(r"^(\d+)_", md_file.name)
                if match:
                    task_id = int(match.group(1))
                    task_folders[task_id] = entry
                    break
    return task_folders

def main():
    # 1. Отримуємо результат виконання квері
    query_url = f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/wit/wiql/{QUERY_ID}?api-version=7.0"
    res = requests.get(query_url, auth=auth, headers=headers)
    res.raise_for_status()
    query_data = res.json()

    work_items = query_data.get("workItems", [])
    if not work_items:
        print("Задач за цією кверею не знайдено.")
        clear_output_dir(OUTPUT_DIR)
        return

    ids = [item["id"] for item in work_items]
    print(f"Знайдено {len(ids)} задач. Завантаження деталей...")

    # 2. Отримуємо поля знайдених Work Items (батчами до 200 штук)
    batch_url = f"https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_apis/wit/workitemsbatch?api-version=7.0"
    payload = {
        "ids": ids,
        "fields": [
            "System.Id",
            "System.WorkItemType",
            "System.Title",
            "System.State",
            "Microsoft.VSTS.Common.Priority",
            "System.Description",
            "Microsoft.VSTS.TCM.ReproSteps",
            "System.AssignedTo"
        ]
    }

    details_res = requests.post(batch_url, json=payload, auth=auth, headers=headers)
    details_res.raise_for_status()
    items = details_res.json().get("value", [])

    # Очищаємо папку перед записом нових файлів
    clear_output_dir(OUTPUT_DIR)

    # 3. Зберігаємо кожен тікет як .md, рахуємо статуси та збираємо задачі в роботу
    state_counter = Counter()
    actionable_tasks = []

    for item in items:
        f = item.get("fields", {})
        task_id = f.get("System.Id")
        item_type = f.get("System.WorkItemType", "Task")
        raw_title = f.get("System.Title", "")
        title = raw_title.replace("/", "-").replace("\\", "-")
        state = f.get("System.State", "Unknown")
        priority = f.get("Microsoft.VSTS.Common.Priority", "N/A")
        
        state_counter[state] += 1

        # Для багів опис зазвичай у ReproSteps, для тасок — у Description
        description = clean_html(f.get("System.Description") or f.get("Microsoft.VSTS.TCM.ReproSteps") or "Немає опису")

        md_content = f"""# [{item_type} #{task_id}] {title}

- **State:** {state}
- **Priority:** {priority}
- **URL:** https://dev.azure.com/{ORGANIZATION}/{PROJECT}/_workitems/edit/{task_id}

## Опис / Кроки відтворення
{description}
"""
        filename = f"{OUTPUT_DIR}/{task_id}_{item_type}.md"
        with open(filename, "w", encoding="utf-8") as out:
            out.write(md_content)

        # Фіксуємо задачі в статусах Active та Proposed
        if state in ("Active", "Proposed"):
            actionable_tasks.append({
                "id": task_id,
                "type": item_type,
                "title": raw_title,
                "state": state,
                "file": filename
            })

    print(f"Готово! Всі задачі вивантажено в папку `{OUTPUT_DIR}/`.\n")
    print("Статистика за статусами:")
    for state, count in state_counter.most_common():
        print(f"  • {state}: {count}")

    # 4. Пошук зіставлених локальних папок
    task_folders = build_task_to_folder_map()

    # 5. Виведення списку для вибору задачі
    if actionable_tasks:
        print("\n" + "=" * 80)
        print("Active / Proposed:")
        print("=" * 80)
        actionable_tasks.sort(key=lambda t: (t["state"] != "Active", t["id"]))
        for t in actionable_tasks:
            folder = task_folders.get(t["id"])
            folder_str = str(folder) if folder else "[не клоновано / папки немає]"

            print(f"[{t['state']}] #{t['id']} ({t['type']}): {t['title']}")
            print(f"  --> MD:     {t['file']}")
            print(f"  --> Folder: {folder_str}\n")
    else:
        print("\nНемає задач у статусах Active або Proposed.")

if __name__ == "__main__":
    main()
