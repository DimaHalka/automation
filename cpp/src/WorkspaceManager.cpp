#include "WorkspaceManager.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <algorithm>
#include <filesystem>

QList<DriveInfo> WorkspaceManager::getDrivesInfo() {
    Settings cfg = Config::loadSettings();
    QList<QDir> roots = Config::getGitRoots(cfg);
    QList<DriveInfo> drives;
    QSet<QString> seen;

    for (const QDir& r : roots) {
        QString nativePath = QDir::toNativeSeparators(r.absolutePath());
        QString drive = nativePath.left(3).toUpper();
        if (seen.contains(drive) || !QDir(drive).exists()) {
            continue;
        }
        seen.insert(drive);

        QStorageInfo storage(drive);
        if (storage.isValid() && storage.isReady() && storage.bytesTotal() > 0) {
            double totalGb = static_cast<double>(storage.bytesTotal()) / (1024.0 * 1024.0 * 1024.0);
            double freeGb = static_cast<double>(storage.bytesAvailable()) / (1024.0 * 1024.0 * 1024.0);
            double usedPct = ((totalGb - freeGb) / totalGb) * 100.0;

            DriveInfo info;
            info.drive = drive;
            info.root = nativePath;
            info.totalGb = std::round(totalGb * 10.0) / 10.0;
            info.freeGb = std::round(freeGb * 10.0) / 10.0;
            info.usedPct = std::round(usedPct * 10.0) / 10.0;
            drives.append(info);
        }
    }

    std::sort(drives.begin(), drives.end(), [](const DriveInfo& a, const DriveInfo& b) {
        return a.freeGb > b.freeGb;
    });
    return drives;
}

QMap<int, QString> WorkspaceManager::scanTaskFoldersMap() {
    QMap<int, QString> mapping;
    QRegularExpression mdRegex(QStringLiteral("^(\\d+)_.*\\.md$"));

    for (const QDir& root : Config::getGitRoots(Config::loadSettings())) {
        if (!root.exists()) continue;

        QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries) {
            if (entry.fileName().startsWith('.')) continue;

            QDir subDir(entry.absoluteFilePath());
            QFileInfoList mdFiles = subDir.entryInfoList(QStringList() << "*_*.md", QDir::Files);
            for (const QFileInfo& md : mdFiles) {
                QRegularExpressionMatch m = mdRegex.match(md.fileName());
                if (m.hasMatch()) {
                    int taskId = m.captured(1).toInt();
                    mapping.insert(taskId, QDir::toNativeSeparators(entry.absoluteFilePath()));
                    break;
                }
            }
        }
    }
    return mapping;
}

static int stateSortOrder(const QString& st) {
    if (st.compare("Active", Qt::CaseInsensitive) == 0) return 0;
    if (st.compare("Proposed", Qt::CaseInsensitive) == 0) return 1;
    if (st.compare("New", Qt::CaseInsensitive) == 0) return 2;
    if (st.compare("Resolved", Qt::CaseInsensitive) == 0) return 3;
    if (st.compare("Closed", Qt::CaseInsensitive) == 0) return 4;
    return 9;
}

QList<SprintTask> WorkspaceManager::loadSprintTasks() {
    QDir sprintDir = Config::sprintTasksDir();
    if (!sprintDir.exists()) {
        return {};
    }

    QMap<int, QString> foldersMap = scanTaskFoldersMap();
    QList<SprintTask> tasks;

    QRegularExpression reTitle(QStringLiteral("^#\\s*\\[(\\w+)\\s*#(\\d+)\\]\\s*(.*)$"), QRegularExpression::MultilineOption);
    QRegularExpression reState(QStringLiteral("-\\s*\\*\\*State:\\*\\*\\s*([^\\n\\r]+)"));
    QRegularExpression rePrio(QStringLiteral("-\\s*\\*\\*Priority:\\*\\*\\s*([^\\n\\r]+)"));
    QRegularExpression reUrl(QStringLiteral("-\\s*\\*\\*URL:\\*\\*\\s*([^\\n\\r]+)"));

    QDir base = Config::baseDir();
    QFileInfoList mdFiles = sprintDir.entryInfoList(QStringList() << "*.md", QDir::Files, QDir::Name);

    for (const QFileInfo& fi : mdFiles) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        QString content = QString::fromUtf8(f.readAll());

        SprintTask t;
        t.title = fi.completeBaseName();
        t.absMdPath = QDir::toNativeSeparators(fi.absoluteFilePath());
        t.mdPath = base.relativeFilePath(fi.absoluteFilePath());
        t.content = content;

        QRegularExpressionMatch mTitle = reTitle.match(content);
        if (mTitle.hasMatch()) {
            t.type = mTitle.captured(1).trimmed();
            t.id = mTitle.captured(2).toInt();
            t.title = mTitle.captured(3).trimmed();
        }

        QRegularExpressionMatch mState = reState.match(content);
        if (mState.hasMatch()) t.state = mState.captured(1).trimmed();

        QRegularExpressionMatch mPrio = rePrio.match(content);
        if (mPrio.hasMatch()) t.priority = mPrio.captured(1).trimmed();

        QRegularExpressionMatch mUrl = reUrl.match(content);
        if (mUrl.hasMatch()) t.url = mUrl.captured(1).trimmed();

        t.folder = foldersMap.value(t.id, QString());
        tasks.append(t);
    }

    std::sort(tasks.begin(), tasks.end(), [](const SprintTask& a, const SprintTask& b) {
        int oa = stateSortOrder(a.state);
        int ob = stateSortOrder(b.state);
        if (oa != ob) return oa < ob;
        return a.id > b.id;
    });
    return tasks;
}

bool WorkspaceManager::isClosedState(const QString& state) {
    const QString s = state.trimmed().toLower();
    return s == "resolved" || s == "closed" || s == "done" || s == "completed";
}

static void parseMdTaskInfo(const QString& filePath, QString& outId, QString& outType, QString& outTitle, QString& outState) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return;
    QString content = QString::fromUtf8(f.readAll());

    QRegularExpression reTitle(QStringLiteral("^#\\s*\\[(\\w+)\\s*#(\\d+)\\]\\s*(.*)$"), QRegularExpression::MultilineOption);
    QRegularExpressionMatch mTitle = reTitle.match(content);
    if (mTitle.hasMatch()) {
        outType = mTitle.captured(1).trimmed();
        outId = mTitle.captured(2).trimmed();
        outTitle = mTitle.captured(3).trimmed();
    }

    QRegularExpression reState(QStringLiteral("-\\s*\\*\\*State:\\*\\*\\s*([^\\n\\r]+)"));
    QRegularExpressionMatch mState = reState.match(content);
    if (mState.hasMatch()) {
        outState = mState.captured(1).trimmed();
    }
}

QList<WorkspaceFolder> WorkspaceManager::scanWorkspaces() {
    QList<WorkspaceFolder> results;
    QDir sprintDir = Config::sprintTasksDir();

    for (const QDir& root : Config::getGitRoots(Config::loadSettings())) {
        if (!root.exists()) continue;

        QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries) {
            if (entry.fileName().startsWith('.')) continue;

            QDir folder(entry.absoluteFilePath());
            WorkspaceFolder w;
            w.name = entry.fileName();
            w.path = QDir::toNativeSeparators(entry.absoluteFilePath());
            w.drive = w.path.left(3).toUpper();
            w.taskTitle = w.name;
            w.state = "No .md info";

            QFileInfoList mdList = folder.entryInfoList(QStringList() << "*_*.md", QDir::Files);
            if (mdList.isEmpty()) {
                QFileInfoList allMd = folder.entryInfoList(QStringList() << "*.md", QDir::Files);
                for (const QFileInfo& fi : allMd) {
                    if (fi.fileName().compare("readme.md", Qt::CaseInsensitive) != 0 &&
                        fi.fileName().compare("AI_PROMPT.md", Qt::CaseInsensitive) != 0) {
                        mdList.append(fi);
                    }
                }
            }

            if (!mdList.isEmpty()) {
                w.state = "Unknown";
                parseMdTaskInfo(mdList.first().absoluteFilePath(), w.taskId, w.taskType, w.taskTitle, w.state);

                // Cross-check with latest sprint-tasks/<id>_*.md
                if (!w.taskId.isEmpty() && sprintDir.exists()) {
                    QFileInfoList sprintMatch = sprintDir.entryInfoList(QStringList() << QString("%1_*.md").arg(w.taskId), QDir::Files);
                    if (!sprintMatch.isEmpty()) {
                        QString sId, sType, sTitle, sState;
                        parseMdTaskInfo(sprintMatch.first().absoluteFilePath(), sId, sType, sTitle, sState);
                        if (!sState.isEmpty()) w.state = sState;
                        if (!sTitle.isEmpty()) w.taskTitle = sTitle;
                    }
                }
            }

            w.isClosed = isClosedState(w.state);
            results.append(w);
        }
    }

    std::sort(results.begin(), results.end(), [](const WorkspaceFolder& a, const WorkspaceFolder& b) {
        if (a.isClosed != b.isClosed) return a.isClosed > b.isClosed; // Closed first for easy cleanup
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return results;
}

bool WorkspaceManager::deleteDirectoryForce(const QString& dirPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path rootPath(dirPath.toStdWString());
    if (!fs::exists(rootPath, ec)) {
        return true;
    }

    // Clear read-only attributes on all files (important for Windows .git pack files)
    for (fs::recursive_directory_iterator it(rootPath, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    }
    fs::permissions(rootPath, fs::perms::owner_write, fs::perm_options::add, ec);
    fs::remove_all(rootPath, ec);
    return !fs::exists(rootPath, ec);
}

void WorkspaceManager::openInExplorer(const QString& pathStr) {
    QFileInfo fi(pathStr);
    if (fi.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath()));
    }
}

QString WorkspaceManager::openInCodeEditor(const QString& pathStr) {
    QFileInfo fi(pathStr);
    if (!fi.exists()) return QString();

    for (const QString& cmd : {QStringLiteral("agy"), QStringLiteral("code")}) {
        QString exe = QStandardPaths::findExecutable(cmd);
        if (!exe.isEmpty()) {
            QProcess::startDetached(exe, {fi.absoluteFilePath()});
            return cmd;
        }
    }
    openInExplorer(pathStr);
    return QStringLiteral("Explorer");
}

void WorkspaceManager::openExternalUrl(const QString& urlStr) {
    if (!urlStr.trimmed().isEmpty()) {
        QDesktopServices::openUrl(QUrl(urlStr.trimmed()));
    }
}

QString WorkspaceManager::suggestShortTaskName(int taskId, const QString& title) {
    QRegularExpression wordRe(QStringLiteral("[A-Za-z0-9]+"));
    QRegularExpressionMatchIterator it = wordRe.globalMatch(title);
    QStringList words;
    while (it.hasNext() && words.size() < 2) {
        words.append(it.next().captured(0));
    }
    QString suffix = words.isEmpty() ? QStringLiteral("Task") : words.join('-');
    return QString("%1-%2").arg(taskId).arg(suffix);
}

// ================= TaskSetupRunner =================

TaskSetupRunner::TaskSetupRunner(QObject* parent) : QObject(parent) {
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        while (m_proc->canReadLine()) {
            QString line = QString::fromUtf8(m_proc->readLine()).trimmed();
            // Strip ANSI escape codes if any
            line.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;]*m")));
            if (!line.isEmpty()) {
                emit logLine(line);
            }
        }
    });

    connect(m_proc, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        // Flush remaining output
        QByteArray rem = m_proc->readAllStandardOutput();
        if (!rem.isEmpty()) {
            for (const QString& l : QString::fromUtf8(rem).split('\n', Qt::SkipEmptyParts)) {
                QString clean = l.trimmed();
                clean.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;]*m")));
                if (!clean.isEmpty()) emit logLine(clean);
            }
        }

        if (m_aborted) {
            m_busy = false;
            emit statusChanged("⏹ Зупинено користувачем");
            emit finished(false, "Процес зупинено користувачем.");
            return;
        }

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            m_busy = false;
            m_steps.clear();
            emit logLine(QString("\n❌ Помилка під час виконання кроку (код %1)").arg(exitCode));
            emit statusChanged(QString("❌ Помилка (код %1)").arg(exitCode));
            emit finished(false, QString("Команда завершилася з кодом %1").arg(exitCode));
            return;
        }

        runNextStep();
    });
}

void TaskSetupRunner::generateAiPrompt(const QDir& taskDir, const QString& taskName, const QString& copiedMdName, const Settings& cfg) {
    QString taskRef = !copiedMdName.isEmpty()
        ? QString("Уважно прочитай опис задачі у файлі `%1`.").arg(copiedMdName)
        : QString("Задача: `%1`.").arg(taskName);

    QString content = QString(
R"(# Інструкція для ШІ: Аналіз та виправлення задачі %1

Ти досвідчений C++ інженер, що працює з проєктом %2.
%3

## Робоче оточення
- Репозиторій розташовано у підпапці: `./%4/`
- Робоча гілка вже створена, конфігурація CMake згенерована.
- Шлях до clang-format: `%5`

## Суворі обмеження:
- **Білд та тести:** НЕ запускай і НЕ перевіряй білд та тести самостійно.
- **Git-коміти:** НЕ роби коміти без моєї явної прямої команди.

## Твої завдання:
1. **Локалізація проблеми:**
   - Знайди відповідні C++ класи, методи та структури в директорії `./%4/`, що стосуються проблеми/фічі.
   - За потреби використовуй пошук за ключовими словами з опису задачі.

2. **Аналіз першопричини (Root Cause Analysis):**
   - Поясни, чому виникає описана поведінка або де саме бракує потрібної функціональності.
   - Зверни увагу на володіння пам'яттю, життєвий цикл об'єктів та обробку крайових випадків.

3. **Внесення змін у код:**
   - Запропонуй мінімальні, чисті та безпечні правки.
   - **Форматування:** обов'язково запусти форматування для всіх змінених/доданих C++ файлів (`.cpp`, `.h`, `.hpp`, `.inl`) командою:
     ```cmd
     %5 -i <шлях_до_файлу>
     ```
)").arg(taskName, cfg.project, taskRef, cfg.repositoryId, cfg.clangFormatPath);

    QFile f(taskDir.filePath("AI_PROMPT.md"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(content.toUtf8());
        emit logLine("📄 Створено файл інструкцій для ШІ: AI_PROMPT.md");
    }
}

void TaskSetupRunner::startSetup(const QString& taskName, const QString& relMdPath) {
    if (m_busy) return;

    Settings cfg = Config::loadSettings();
    QList<DriveInfo> drives = WorkspaceManager::getDrivesInfo();
    QString bestRoot = drives.isEmpty()
        ? QDir::toNativeSeparators(QDir::home().filePath("Git"))
        : drives.first().root;

    QDir gitRootDir(bestRoot);
    QDir taskDir(gitRootDir.filePath(taskName));
    m_targetTaskDir = QDir::toNativeSeparators(taskDir.absolutePath());

    if (taskDir.exists()) {
        emit logLine(QString("❌ Директорія вже існує: %1").arg(m_targetTaskDir));
        emit finished(false, QString("Директорія %1 вже існує!").arg(m_targetTaskDir));
        return;
    }

    m_busy = true;
    m_aborted = false;
    m_steps.clear();

    emit statusChanged("⏳ Виконується...");
    emit logLine(QString("[%1] 🚀 Початок налаштування робочого простору: %2")
                     .arg(QTime::currentTime().toString("HH:mm:ss"), m_targetTaskDir));

    if (!drives.isEmpty()) {
        emit logLine(QString("💾 Диск з найбільшим вільним простором: %1 (%2 GB вільно) -> %3")
                         .arg(drives.first().drive)
                         .arg(drives.first().freeGb, 0, 'f', 1)
                         .arg(bestRoot));
    }

    if (!taskDir.mkpath(".")) {
        m_busy = false;
        emit logLine("❌ Не вдалося створити робочу папку!");
        emit finished(false, "Не вдалося створити робочу папку.");
        return;
    }

    // Copy .md file if selected
    QString copiedMdName;
    if (!relMdPath.trimmed().isEmpty()) {
        QString srcMd = Config::baseDir().filePath(relMdPath);
        QFileInfo mdFi(srcMd);
        if (mdFi.exists()) {
            QString dstMd = taskDir.filePath(mdFi.fileName());
            QFile::copy(srcMd, dstMd);
            copiedMdName = mdFi.fileName();
            emit logLine(QString("📋 Скопійовано %1 у %2").arg(copiedMdName, m_targetTaskDir));
        }
    }

    generateAiPrompt(taskDir, taskName, copiedMdName, cfg);

    QString repoUrl = QString("https://dev.azure.com/%1/_git/%2").arg(cfg.organization, cfg.repositoryId);
    QString repoDir = taskDir.filePath(cfg.repositoryId);
    QString branchPrefix = cfg.branchPrefix.isEmpty() ? cfg.repositoryId.toLower() : cfg.branchPrefix;
    m_branchName = QString("%1/%2/%3").arg(branchPrefix, cfg.userPrefix, taskName.toLower());

    // Step 1: git clone
    m_steps.append({
        "Клонування репозиторію",
        "git",
        {"clone", "--progress", "--depth", "100", repoUrl},
        taskDir.absolutePath()
    });

    // Step 2: git submodule update
    m_steps.append({
        "Оновлення субмодулів",
        "git",
        {"submodule", "update", "--init", "--recursive", "--progress", "--jobs", "8", "--depth", "10"},
        repoDir
    });

    // Step 3: git checkout -b
    m_steps.append({
        QString("Створення робочої гілки %1").arg(m_branchName),
        "git",
        {"checkout", "-b", m_branchName},
        repoDir
    });

    // Step 4: Build/CMake.bat NOPAUSE (executed via cmd.exe /c if exists)
    QString cmakeBat = QDir(repoDir).filePath("Build/CMake.bat");
    m_steps.append({
        "Запуск генерації проєкту (Build\\CMake.bat NOPAUSE)",
        "cmd.exe",
        {"/c", "if exist Build\\CMake.bat (cd /d Build && call CMake.bat NOPAUSE) else (echo Попередження: Build\\CMake.bat не знайдено)"},
        repoDir
    });

    runNextStep();
}

void TaskSetupRunner::runNextStep() {
    if (m_steps.isEmpty()) {
        m_busy = false;
        emit logLine("\n============================================================");
        emit logLine(QString("✅ Усе готово! Робочий простір: %1").arg(m_targetTaskDir));
        emit logLine(QString("🌿 Гілка: %1").arg(m_branchName));
        emit logLine("============================================================");
        emit statusChanged("✅ Готово");
        emit finished(true, "Робоче оточення успішно створено!");
        return;
    }

    Step step = m_steps.takeFirst();
    emit logLine(QString("\n▶ %1").arg(step.description));
    emit logLine(QString("  $ %1 %2").arg(step.program, step.args.join(' ')));

    m_proc->setWorkingDirectory(step.workingDir);
    m_proc->start(step.program, step.args);
}

void TaskSetupRunner::stop() {
    if (m_busy && m_proc->state() != QProcess::NotRunning) {
        m_aborted = true;
        m_steps.clear();
        m_proc->kill();
    }
}
