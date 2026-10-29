#pragma once

#include "Config.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>

struct DriveInfo {
    QString drive;
    QString root;
    double totalGb = 0.0;
    double freeGb = 0.0;
    double usedPct = 0.0;
};

struct SprintTask {
    int id = 0;
    QString type = "Task";
    QString title;
    QString state = "Unknown";
    QString priority = "N/A";
    QString url;
    QString mdPath;     // relative to baseDir
    QString absMdPath;
    QString content;
    QString folder;     // local workspace path if cloned
};

struct WorkspaceFolder {
    QString name;
    QString path;
    QString drive;
    QString taskId;
    QString taskType = "Task";
    QString taskTitle;
    QString state = "Unknown";
    bool isClosed = false;
};

class WorkspaceManager {
public:
    static QList<DriveInfo> getDrivesInfo();
    static QMap<int, QString> scanTaskFoldersMap();
    static QList<SprintTask> loadSprintTasks();
    static QList<WorkspaceFolder> scanWorkspaces();

    static bool isClosedState(const QString& state);
    static bool deleteDirectoryForce(const QString& dirPath);

    static void openInExplorer(const QString& pathStr);
    static QString openInCodeEditor(const QString& pathStr);
    static void openExternalUrl(const QString& urlStr);

    static QString suggestShortTaskName(int taskId, const QString& title);
};

class TaskSetupRunner : public QObject {
    Q_OBJECT
public:
    explicit TaskSetupRunner(QObject* parent = nullptr);

    bool isBusy() const { return m_busy; }
    void startSetup(const QString& taskName, const QString& relMdPath);
    void stop();

signals:
    void logLine(const QString& line);
    void statusChanged(const QString& statusText);
    void finished(bool success, const QString& message);

private:
    struct Step {
        QString description;
        QString program;
        QStringList args;
        QString workingDir;
    };

    void runNextStep();
    void generateAiPrompt(const QDir& taskDir, const QString& taskName, const QString& copiedMdName, const Settings& cfg);

    bool m_busy = false;
    bool m_aborted = false;
    QProcess* m_proc = nullptr;
    QList<Step> m_steps;
    QString m_targetTaskDir;
    QString m_branchName;
};
