import os
import re
import shutil
import stat
from pathlib import Path
from config import get_git_roots

# Можливі кореневі директорії з робочими папками
GIT_ROOTS = get_git_roots()

SPRINT_TASKS_DIR = Path("sprint-tasks")
CLOSED_STATES = {"resolved", "closed", "done", "completed"}


def remove_readonly(func, path, _):
    """Скидає атрибут Read-only під Windows для видалення файлів .git."""
    try:
        os.chmod(path, stat.S_IWRITE)
        func(path)
    except Exception:
        pass


def parse_md_file(file_path: Path) -> dict:
    """Витягує назву, статус та ID з md-файлу задачі."""
    info = {"title": "Невідома назва", "state": "Unknown", "id": None, "type": "Task"}
    if not file_path.exists():
        return info

    try:
        content = file_path.read_text(encoding="utf-8")
        title_match = re.search(r"^#\s*\[(\w+)\s*#(\d+)\]\s*(.*)$", content, re.MULTILINE)
        if title_match:
            info["type"] = title_match.group(1)
            info["id"] = title_match.group(2)
            info["title"] = title_match.group(3).strip()

        state_match = re.search(r"-\s*\*\*State:\*\*\s*([^\n\r]+)", content)
        if state_match:
            info["state"] = state_match.group(1).strip()
    except Exception:
        pass

    return info


def get_task_info_from_folder(folder: Path) -> dict:
    """Знаходить md-файл у папці та звіряє його статус зі sprint-tasks."""
    md_files = list(folder.glob("*_*.md")) + [f for f in folder.glob("*.md") if f.name.lower() != "readme.md"]

    if not md_files:
        return {"title": folder.name, "state": "No .md info", "id": None}

    info = parse_md_file(md_files[0])

    if info["id"] and SPRINT_TASKS_DIR.exists():
        matching = list(SPRINT_TASKS_DIR.glob(f"{info['id']}_*.md"))
        if matching:
            sprint_info = parse_md_file(matching[0])
            info["state"] = sprint_info["state"]
            info["title"] = sprint_info["title"]

    return info


def get_dir_size_gb(folder: Path) -> float:
    """Рахує розмір каталогу в GB."""
    total_bytes = 0
    try:
        for root, _, files in os.walk(folder):
            for f in files:
                try:
                    total_bytes += os.path.getsize(os.path.join(root, f))
                except OSError:
                    pass
    except Exception:
        pass
    return total_bytes / (1024**3)


def main():
    print("Сканування робочих директорій...\n")
    folders = []

    for root in GIT_ROOTS:
        if not root.exists():
            continue
        for entry in root.iterdir():
            if entry.is_dir() and not entry.name.startswith("."):
                folders.append(entry)

    if not folders:
        print("Робочих папок не знайдено.")
        return

    # Збираємо інформацію по всіх папках
    items = []
    for f in folders:
        info = get_task_info_from_folder(f)
        size_gb = get_dir_size_gb(f)
        items.append({"path": f, "info": info, "size_gb": size_gb})

    # Виводимо нумерований список
    print(f"{'#':<3} | {'Папка':<25} | {'Розмір':<9} | {'Статус':<12} | {'Задача':<70}")
    print("-" * 110)
    for idx, item in enumerate(items, start=1):
        f_name = item["path"].name
        if len(f_name) > 23:
            f_name = f_name[:20] + "..."

        size_str = f"{item['size_gb']:.1f} GB"
        state_str = item["info"]["state"]
        
        # Додаємо маркер для закритих
        if state_str.lower() in CLOSED_STATES:
            state_str = f"[x] {state_str}"

        task_str = ""
        if item["info"]["id"]:
            task_str = f"#{item['info']['id']}: {item['info']['title']}"
        elif item["info"]["title"] != item["path"].name:
            task_str = item["info"]["title"]

        # Обрізаємо опис задачі рівно до 70 символів
        if len(task_str) > 70:
            task_str = task_str[:67] + "..."

        print(f"{idx:<3} | {f_name:<25} | {size_str:<9} | {state_str:<12} | {task_str:<70}")

    print("-" * 110)
    user_input = input("\nВведіть номер(и) папок для видалення (наприклад: 1 або 1, 3) [Enter для виходу]: ").strip()
    if not user_input:
        print("Скасовано.")
        return

    # Парсимо номери
    indices = []
    for part in re.split(r"[\s,]+", user_input):
        if part.isdigit():
            idx = int(part)
            if 1 <= idx <= len(items):
                indices.append(idx - 1)

    if not indices:
        print("Не вибрано жодного коректного номера.")
        return

    # Підтвердження операції
    to_delete = [items[i] for i in sorted(set(indices))]
    total_freed = sum(it["size_gb"] for it in to_delete)

    print("\nДо видалення вибрано:")
    for it in to_delete:
        print(f"  • {it['path']} ({it['size_gb']:.1f} GB)")

    confirm = input(f"\nЗвільниться близько {total_freed:.1f} GB. Підтверджуєте видалення? [y/N]: ").strip().lower()
    if confirm in ("y", "yes"):
        for it in to_delete:
            target = it["path"]
            print(f"Видалення {target}...")
            shutil.rmtree(target, onerror=remove_readonly)
        print("\nВидалення завершено успішно.")
    else:
        print("Видалення скасовано.")


if __name__ == "__main__":
    main()
