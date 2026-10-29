import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from config import get_git_roots, load_settings

_CFG = load_settings()
ORGANIZATION = _CFG["organization"]
PROJECT = _CFG["project"]
REPOSITORY_ID = _CFG["repository_id"]
REPO_URL = f"https://dev.azure.com/{ORGANIZATION}/_git/{REPOSITORY_ID}"
USER_PREFIX = _CFG["user_prefix"]
BRANCH_PREFIX = _CFG.get("branch_prefix", REPOSITORY_ID.lower())
CLANG_FORMAT_PATH = _CFG.get("clang_format_path", "clang-format.exe")
GIT_ROOTS = get_git_roots(_CFG)


def get_best_git_root() -> Path:
    """Вибирає серед налаштованих git_roots папку на диску з найбільшою кількістю вільного місця."""
    candidates = []
    for root in GIT_ROOTS:
        drive = root.anchor or str(root)[:3]
        if os.path.exists(drive):
            try:
                free_gb = shutil.disk_usage(drive).free / (1024**3)
                candidates.append((root, drive, free_gb))
            except Exception:
                pass

    if not candidates:
        return Path.home() / "Git"

    candidates.sort(key=lambda x: x[2], reverse=True)
    best_root, best_drive, best_free = candidates[0]
    print(f"Диск з найбільшим вільним простором: {best_drive} ({best_free:.1f} GB вільно) -> {best_root}")
    return best_root


def run_command(cmd, cwd=None):
    """Виконує команду в терміналі з виводом stdout/stderr у реальному часі."""
    print(f"\n[EXEC] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str))
    if res.returncode != 0:
        print(f"Помилка під час виконання команди! Код: {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)


def generate_ai_prompt(task_dir: Path, task_name: str, md_filename: str | None):
    """Створює AI_PROMPT.md у корені робочої папки з готовими інструкціями для ШІ."""
    task_ref = f"Уважно прочитай опис задачі у файлі `{md_filename}`." if md_filename else f"Задача: `{task_name}`."
    
    prompt_content = f"""# Інструкція для ШІ: Аналіз та виправлення задачі {task_name}

Ти досвідчений C++ інженер, що працює з проєктом {PROJECT}.
{task_ref}

## Робоче оточення
- Репозиторій розташовано у підпапці: `./{REPOSITORY_ID}/`
- Робоча гілка вже створена, конфігурація CMake згенерована.
- Шлях до clang-format: `{CLANG_FORMAT_PATH}`

## Суворі обмеження:
- **Білд та тести:** НЕ запускай і НЕ перевіряй білд та тести самостійно.
- **Git-коміти:** НЕ роби коміти без моєї явної прямої команди.

## Твої завдання:
1. **Локалізація проблеми:**
   - Знайди відповідні C++ класи, методи та структури в директорії `./{REPOSITORY_ID}/`, що стосуються проблеми/фічі.
   - За потреби використовуй пошук за ключовими словами з опису задачі.

2. **Аналіз першопричини (Root Cause Analysis):**
   - Поясни, чому виникає описана поведінка або де саме бракує потрібної функціональності.
   - Зверни увагу на володіння пам'яттю, життєвий цикл об'єктів та обробку крайових випадків.

3. **Внесення змін у код:**
   - Запропонуй мінімальні, чисті та безпечні правки.
   - **Форматування:** обов'язково запусти форматування для всіх змінених/доданих C++ файлів (`.cpp`, `.h`, `.hpp`, `.inl`) командою:
     ```cmd
     {CLANG_FORMAT_PATH} -i <шлях_до_файлу>
     ```
"""
    prompt_file = task_dir / "AI_PROMPT.md"
    prompt_file.write_text(prompt_content, encoding="utf-8")
    print(f"Створено файл інструкцій для ШІ: {prompt_file.name}")


def main():
    parser = argparse.ArgumentParser(description="Автоматичне налаштування робочого простору під задачу")
    parser.add_argument("-t", "--task", help="Коротка назва задачі (наприклад: Oste-Rotate)")
    parser.add_argument("-m", "--md", help="Шлях до файлу задачі (наприклад: sprint-tasks/12345_Bug.md)")
    args = parser.parse_args()

    # 1. Отримання вхідних даних
    task_name = args.task
    if not task_name:
        task_name = input("Введіть коротку назву задачі (наприклад, Oste-Rotate): ").strip()
    if not task_name:
        print("Помилка: назва задачі не може бути порожньою.", file=sys.stderr)
        sys.exit(1)

    md_path_str = args.md
    if not md_path_str:
        md_path_str = input("Введіть шлях до .md файлу задачі (Enter для пропуску): ").strip()

    md_path = Path(md_path_str) if md_path_str else None
    if md_path and not md_path.exists():
        print(f"Попередження: файл {md_path} не знайдено. Продовжуємо без копіювання файлу.")
        md_path = None

    # 2. Визначення кореневої директорії та створення папки задачі
    git_root = get_best_git_root()
    task_dir = git_root / task_name

    if task_dir.exists():
        print(f"Помилка: директорія {task_dir} вже існує. Виберіть інше ім'я або видаліть її вручну.", file=sys.stderr)
        sys.exit(1)

    print(f"Створення робочої директорії: {task_dir}")
    task_dir.mkdir(parents=True, exist_ok=True)

    # Копіюємо .md файл задачі прямо в корінь робочої папки
    copied_md_name = None
    if md_path:
        shutil.copy(md_path, task_dir / md_path.name)
        copied_md_name = md_path.name
        print(f"Скопійовано {md_path.name} в {task_dir}")

    # Копіюємо create-pr.py в робочу папку (шукаємо поруч зі скриптом або в поточній директорії)
    pr_script_candidates = [
        Path(__file__).parent / "create-pr.py",
        Path.cwd() / "create-pr.py"
    ]
    pr_script_source = next((p for p in pr_script_candidates if p.exists()), None)
    if pr_script_source:
        shutil.copy(pr_script_source, task_dir / "create-pr.py")
        print(f"Скопійовано create-pr.py в {task_dir}")
    else:
        print("Попередження: create-pr.py не знайдено поруч зі скриптом, файл не скопійовано.", file=sys.stderr)

    # Створюємо файл інструкцій для AI
    generate_ai_prompt(task_dir, task_name, copied_md_name)

    # 3. Клонування репозиторію
    run_command(["git", "clone", "--depth", "100", REPO_URL], cwd=str(task_dir))

    repo_dir = task_dir / REPOSITORY_ID

    # 4. Оновлення субмодулів
    run_command(
        ["git", "submodule", "update", "--init", "--recursive", "--progress", "--jobs", "8", "--depth", "10"],
        cwd=str(repo_dir)
    )

    # 5. Створення гілки під задачу
    branch_name = f"{BRANCH_PREFIX}/{USER_PREFIX}/{task_name.lower()}"
    print(f"\nСтворення робочої гілки: {branch_name}")
    run_command(["git", "checkout", "-b", branch_name], cwd=str(repo_dir))

    # 6. Запуск CMake.bat з ключем NOPAUSE
    build_script = repo_dir / "Build" / "CMake.bat"
    if build_script.exists():
        print(f"\nЗапуск генерації проєкту через {build_script.name} NOPAUSE...")
        run_command([str(build_script), "NOPAUSE"], cwd=str(build_script.parent))
    else:
        print(f"Попередження: {build_script} не знайдено!", file=sys.stderr)

    print("\n" + "=" * 60)
    print(f"Усе готово! Робочий простір: {task_dir}")
    print(f"Гілка: {branch_name}")
    print(f"Файли в корені: .md опис, AI_PROMPT.md, create-pr.py")
    print("=" * 60)


if __name__ == "__main__":
    main()
