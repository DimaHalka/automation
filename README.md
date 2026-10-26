# Automation Hub

Локальний дешборд та набір Python-скриптів для автоматизації щоденного робочого процесу з Azure DevOps та Git.

## Можливості

- **Таски спринту (`sync-sprint.py`)** — синхронізація робочих задач із Azure DevOps у локальні `.md` файли (`sprint-tasks/`), швидкий перегляд опису та статусу клонованих папок.
- **Робоче оточення (`task-setup.py`, `create-pr.py`)** — автоматичний вибір диска з найбільшим вільним простором, клонування репозиторію та субмодулів, генерація `AI_PROMPT.md`, запуск CMake та швидке створення Pull Request.
- **Keep Space (`keep-space.py`)** — аналіз розміру робочих директорій у `git_roots`, підсвітка закритих задач (`Resolved` / `Closed`) та очищення місця на диску в один клік.
- **Моніторинг PR (`watch-pr.py`)** — фонове відстеження Gated-білдів, нових коментарів та голосів рев'юверів із системними сповіщеннями Windows (Toast).
- **Швидкі лінки та Налаштування (`data/links.json`, `data/settings.json`)** — локальне збереження закладок і параметрів без засмічення Git-репозиторію.

## Швидкий старт

1. Встановіть залежності:
   ```powershell
   pip install -r requirements.txt
   ```
2. Скопіюйте приклад конфігурації у локальну папку `data/` (яка ігнорується Git):
   ```powershell
   mkdir data -Force
   copy settings.example.json data\settings.json
   ```
3. Переконайтеся, що змінна середовища `ADO_PAT` встановлена (Personal Access Token для Azure DevOps):
   ```powershell
   $env:ADO_PAT="ваш_токен"
   ```
4. Запустіть UI-дешборд:
   ```powershell
   python dashboard.py
   ```
   або двічі клікніть по `run-dashboard.bat`. Дешборд відкриється за адресою `http://127.0.0.1:8420`.
