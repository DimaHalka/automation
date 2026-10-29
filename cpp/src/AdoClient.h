#pragma once

#include "Config.h"

#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QString>

struct PrComment {
    int threadId = 0;
    int commentId = 0;
    QString author;
    QString date;
    QString content;
};

struct PrActiveThread {
    int threadId = 0;
    QString status;
    QString lastAuthor;
    QString lastSnippet;
};

struct PrSnapshot {
    int id = 0;
    QString title;
    QString branch;
    QString projectId;
    QString url;
    QMap<QString, QString> builds;                  // buildDisplayName -> status
    QMap<QPair<int, int>, PrComment> commentsMap;   // (threadId, commentId) -> comment
    QList<PrActiveThread> activeThreads;
    QMap<QString, int> reviewers;                   // displayName -> vote
};

struct PrEvent {
    QString time;
    QString text;
    QString notifyTitle;
    QString notifyBody;
    bool isError = false;
};

class AdoClient : public QObject {
    Q_OBJECT
public:
    explicit AdoClient(QObject* parent = nullptr);

    bool isSyncingSprint() const { return m_syncingSprint; }
    bool isFetchingPrs() const { return m_fetchingPrs; }

    void syncSprintTasks();
    void fetchPullRequests(bool includeDrafts);

    // Inspects git branch and default PR title for a local workspace folder
    static bool inspectWorkspaceForPr(const QString& workspacePath, QString& outBranch, QString& outDefaultTitle, int& outTaskId, QString& outError);
    void createPullRequest(const QString& workspacePath, const QString& prTitle);

    static QList<PrEvent> compareSnapshots(const QMap<int, PrSnapshot>& oldSnaps, const QMap<int, PrSnapshot>& newSnaps);
    static QString cleanHtml(const QString& rawHtml);

signals:
    void sprintSyncLog(const QString& line);
    void sprintSyncFinished(bool success, const QString& message);

    void prSnapshotsReady(const QMap<int, PrSnapshot>& snapshots);
    void prFetchError(const QString& errorText);

    void createPrLog(const QString& line);
    void createPrFinished(bool success, const QString& prUrl, const QString& message);

private:
    QNetworkRequest makeAdoRequest(const QString& urlStr, const QString& pat) const;
    void fetchPrDetailsBatch(const QList<PrSnapshot>& baseList, const Settings& cfg, const QString& pat);

    QNetworkAccessManager* m_nam = nullptr;
    QString m_cachedUserId;
    bool m_syncingSprint = false;
    bool m_fetchingPrs = false;
};
