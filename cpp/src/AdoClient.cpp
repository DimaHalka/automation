#include "AdoClient.h"
#include "WorkspaceManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <memory>

AdoClient::AdoClient(QObject* parent) : QObject(parent) {
    m_nam = new QNetworkAccessManager(this);
}

QNetworkRequest AdoClient::makeAdoRequest(const QString& urlStr, const QString& pat) const {
    QNetworkRequest req{QUrl(urlStr)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QByteArray authBytes = QString(":%1").arg(pat).toUtf8().toBase64();
    req.setRawHeader("Authorization", "Basic " + authBytes);
    req.setTransferTimeout(20000);
    return req;
}

QString AdoClient::cleanHtml(const QString& rawHtml) {
    if (rawHtml.isEmpty()) return QString();
    QString text = rawHtml;
    text.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption), "\n");
    text.replace(QRegularExpression(QStringLiteral("</p>"), QRegularExpression::CaseInsensitiveOption), "\n\n");
    text.replace(QRegularExpression(QStringLiteral("<[^>]+>")), "");
    text.replace("&nbsp;", " ");
    text.replace("&amp;", "&");
    text.replace("&lt;", "<");
    text.replace("&gt;", ">");
    text.replace("&quot;", "\"");
    text.replace("&#39;", "'");
    return text.trimmed();
}

// ================= 1. SPRINT SYNC (sync-sprint.py replacement) =================

void AdoClient::syncSprintTasks() {
    if (m_syncingSprint) return;

    QString pat = Config::getAdoPat();
    if (pat.isEmpty()) {
        emit sprintSyncLog("❌ Помилка: змінна середовища ADO_PAT не встановлена.");
        emit sprintSyncFinished(false, "ADO_PAT відсутній");
        return;
    }

    Settings cfg = Config::loadSettings();
    if (cfg.queryId.trimmed().isEmpty()) {
        emit sprintSyncLog("❌ Помилка: Query ID не вказано в налаштуваннях (data/settings.json).");
        emit sprintSyncFinished(false, "Query ID порожній");
        return;
    }

    m_syncingSprint = true;
    emit sprintSyncLog(QString("[%1] 🔄 Запит списку задач за WIQL Query (%2)...")
                           .arg(QTime::currentTime().toString("HH:mm:ss"), cfg.queryId));

    QString queryUrl = QString("https://dev.azure.com/%1/%2/_apis/wit/wiql/%3?api-version=7.0")
                           .arg(cfg.organization, cfg.project, cfg.queryId);

    QNetworkReply* reply = m_nam->get(makeAdoRequest(queryUrl, pat));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cfg, pat]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_syncingSprint = false;
            QString err = QString("Помилка WIQL запиту: %1").arg(reply->errorString());
            emit sprintSyncLog("❌ " + err);
            emit sprintSyncFinished(false, err);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray workItems = doc.object()["workItems"].toArray();

        QDir outDir = Config::sprintTasksDir();
        if (workItems.isEmpty()) {
            if (outDir.exists()) outDir.removeRecursively();
            outDir.mkpath(".");
            m_syncingSprint = false;
            emit sprintSyncLog("ℹ️ Задач за цією кверею не знайдено.");
            emit sprintSyncFinished(true, "Задач не знайдено");
            return;
        }

        QJsonArray idsArr;
        for (const QJsonValue& v : workItems) {
            int id = v.toObject()["id"].toInt();
            if (id > 0) idsArr.append(id);
        }

        emit sprintSyncLog(QString("📦 Знайдено %1 задач. Завантаження деталей...").arg(idsArr.size()));

        QString batchUrl = QString("https://dev.azure.com/%1/%2/_apis/wit/workitemsbatch?api-version=7.0")
                               .arg(cfg.organization, cfg.project);

        QJsonObject payload;
        payload["ids"] = idsArr;
        payload["fields"] = QJsonArray{
            "System.Id",
            "System.WorkItemType",
            "System.Title",
            "System.State",
            "Microsoft.VSTS.Common.Priority",
            "System.Description",
            "Microsoft.VSTS.TCM.ReproSteps",
            "System.AssignedTo"
        };

        QNetworkReply* batchReply = m_nam->post(makeAdoRequest(batchUrl, pat), QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(batchReply, &QNetworkReply::finished, this, [this, batchReply, cfg]() {
            batchReply->deleteLater();
            m_syncingSprint = false;

            if (batchReply->error() != QNetworkReply::NoError) {
                QString err = QString("Помилка завантаження деталей задач: %1").arg(batchReply->errorString());
                emit sprintSyncLog("❌ " + err);
                emit sprintSyncFinished(false, err);
                return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(batchReply->readAll());
            QJsonArray items = doc.object()["value"].toArray();

            QDir outDir = Config::sprintTasksDir();
            if (outDir.exists()) outDir.removeRecursively();
            outDir.mkpath(".");

            QMap<QString, int> stateCounts;
            for (const QJsonValue& val : items) {
                QJsonObject f = val.toObject()["fields"].toObject();
                int taskId = f["System.Id"].toInt();
                QString itemType = f["System.WorkItemType"].toString("Task");
                QString rawTitle = f["System.Title"].toString();
                QString cleanTitle = rawTitle;
                cleanTitle.replace('/', '-').replace('\\', '-');
                QString state = f["System.State"].toString("Unknown");
                QString priority = f.contains("Microsoft.VSTS.Common.Priority")
                                       ? QString::number(f["Microsoft.VSTS.Common.Priority"].toInt())
                                       : QStringLiteral("N/A");

                stateCounts[state]++;

                QString rawDesc = f["System.Description"].toString();
                if (rawDesc.isEmpty()) {
                    rawDesc = f["Microsoft.VSTS.TCM.ReproSteps"].toString();
                }
                QString description = cleanHtml(rawDesc);
                if (description.isEmpty()) description = "Немає опису";

                QString mdContent = QString(
R"(# [%1 #%2] %3

- **State:** %4
- **Priority:** %5
- **URL:** https://dev.azure.com/%6/%7/_workitems/edit/%2

## Опис / Кроки відтворення
%8
)").arg(itemType, QString::number(taskId), cleanTitle, state, priority, cfg.organization, cfg.project, description);

                QString filename = outDir.filePath(QString("%1_%2.md").arg(taskId).arg(itemType));
                QFile outFile(filename);
                if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    outFile.write(mdContent.toUtf8());
                }
            }

            emit sprintSyncLog(QString("✅ Вивантажено %1 задач у папку sprint-tasks/").arg(items.size()));
            for (auto it = stateCounts.cbegin(); it != stateCounts.cend(); ++it) {
                emit sprintSyncLog(QString("   • %1: %2").arg(it.key()).arg(it.value()));
            }
            emit sprintSyncFinished(true, QString("Синхронізовано %1 задач").arg(items.size()));
        });
    });
}

// ================= 2. PR MONITOR (watch-pr.py replacement) =================

void AdoClient::fetchPullRequests(bool includeDrafts) {
    if (m_fetchingPrs) return;

    QString pat = Config::getAdoPat();
    if (pat.isEmpty()) {
        emit prFetchError("Змінна середовища ADO_PAT не встановлена");
        return;
    }

    Settings cfg = Config::loadSettings();
    m_fetchingPrs = true;

    auto queryActivePrs = [this, cfg, pat, includeDrafts](const QString& userId) {
        QString url = QString("https://dev.azure.com/%1/%2/_apis/git/repositories/%3/pullrequests"
                              "?searchCriteria.creatorId=%4&searchCriteria.status=active&api-version=7.1")
                          .arg(cfg.organization, cfg.project, cfg.repositoryId, userId);

        QNetworkReply* reply = m_nam->get(makeAdoRequest(url, pat));
        connect(reply, &QNetworkReply::finished, this, [this, reply, cfg, pat, includeDrafts]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                m_fetchingPrs = false;
                emit prFetchError(reply->errorString());
                return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonArray prsArr = doc.object()["value"].toArray();
            QList<PrSnapshot> baseList;

            for (const QJsonValue& v : prsArr) {
                QJsonObject pr = v.toObject();
                if (!includeDrafts && pr["isDraft"].toBool(false)) {
                    continue;
                }

                PrSnapshot snap;
                snap.id = pr["pullRequestId"].toInt();
                snap.title = pr["title"].toString();
                snap.branch = pr["sourceRefName"].toString().replace("refs/heads/", "");
                snap.projectId = pr["repository"].toObject()["project"].toObject()["id"].toString();
                snap.url = QString("https://dev.azure.com/%1/%2/_git/%3/pullrequest/%4")
                               .arg(cfg.organization, cfg.project, cfg.repositoryId)
                               .arg(snap.id);

                for (const QJsonValue& rv : pr["reviewers"].toArray()) {
                    QJsonObject rObj = rv.toObject();
                    QString rName = rObj["displayName"].toString("Unknown");
                    int vote = rObj["vote"].toInt(0);
                    snap.reviewers[rName] = vote;
                }

                baseList.append(snap);
            }

            if (baseList.isEmpty()) {
                m_fetchingPrs = false;
                emit prSnapshotsReady({});
                return;
            }

            fetchPrDetailsBatch(baseList, cfg, pat);
        });
    };

    if (!m_cachedUserId.isEmpty()) {
        queryActivePrs(m_cachedUserId);
        return;
    }

    QString connUrl = QString("https://dev.azure.com/%1/_apis/connectionData").arg(cfg.organization);
    QNetworkReply* connReply = m_nam->get(makeAdoRequest(connUrl, pat));
    connect(connReply, &QNetworkReply::finished, this, [this, connReply, queryActivePrs]() {
        connReply->deleteLater();
        if (connReply->error() != QNetworkReply::NoError) {
            m_fetchingPrs = false;
            emit prFetchError("Помилка автентифікації в ADO: " + connReply->errorString());
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(connReply->readAll());
        QString userId = doc.object()["authenticatedUser"].toObject()["id"].toString();
        if (userId.isEmpty()) {
            m_fetchingPrs = false;
            emit prFetchError("Не вдалося отримати ID користувача з ADO connectionData");
            return;
        }
        m_cachedUserId = userId;
        queryActivePrs(userId);
    });
}

void AdoClient::fetchPrDetailsBatch(const QList<PrSnapshot>& baseList, const Settings& cfg, const QString& pat) {
    struct BatchState {
        QMap<int, PrSnapshot> results;
        int pendingRequests = 0;
    };

    auto state = std::make_shared<BatchState>();
    state->pendingRequests = baseList.size() * 2;
    for (const PrSnapshot& s : baseList) {
        state->results.insert(s.id, s);
    }

    auto checkComplete = [this, state]() {
        state->pendingRequests--;
        if (state->pendingRequests <= 0) {
            m_fetchingPrs = false;
            emit prSnapshotsReady(state->results);
        }
    };

    for (const PrSnapshot& snap : baseList) {
        int prId = snap.id;

        // 1. Policy evaluations (Gated Builds)
        QString evalUrl = QString("https://dev.azure.com/%1/%2/_apis/policy/evaluations"
                                  "?artifactId=vstfs:///CodeReview/CodeReviewId/%3/%4&api-version=7.1-preview.1")
                              .arg(cfg.organization, cfg.project, snap.projectId)
                              .arg(prId);

        QNetworkReply* evalReply = m_nam->get(makeAdoRequest(evalUrl, pat));
        connect(evalReply, &QNetworkReply::finished, this, [evalReply, state, prId, checkComplete]() {
            evalReply->deleteLater();
            if (evalReply->error() == QNetworkReply::NoError) {
                QJsonDocument doc = QJsonDocument::fromJson(evalReply->readAll());
                for (const QJsonValue& v : doc.object()["value"].toArray()) {
                    QJsonObject ev = v.toObject();
                    QJsonObject conf = ev["configuration"].toObject();
                    QString cType = conf["type"].toObject()["displayName"].toString();
                    if (cType == "Build") {
                        QString dispName = conf["settings"].toObject()["displayName"].toString();
                        if (dispName.isEmpty()) dispName = "Build";
                        QString status = ev["status"].toString("unknown");
                        state->results[prId].builds[dispName] = status;
                    }
                }
            }
            checkComplete();
        });

        // 2. Threads & Comments
        QString thUrl = QString("https://dev.azure.com/%1/%2/_apis/git/repositories/%3/pullrequests/%4/threads?api-version=7.1")
                            .arg(cfg.organization, cfg.project, cfg.repositoryId)
                            .arg(prId);

        QNetworkReply* thReply = m_nam->get(makeAdoRequest(thUrl, pat));
        connect(thReply, &QNetworkReply::finished, this, [thReply, state, prId, checkComplete]() {
            thReply->deleteLater();
            if (thReply->error() == QNetworkReply::NoError) {
                QJsonDocument doc = QJsonDocument::fromJson(thReply->readAll());
                for (const QJsonValue& v : doc.object()["value"].toArray()) {
                    QJsonObject th = v.toObject();
                    if (th["isDeleted"].toBool(false)) continue;

                    int thId = th["id"].toInt();
                    QString thStatus = th["status"].toString();

                    QList<PrComment> userComments;
                    for (const QJsonValue& cv : th["comments"].toArray()) {
                        QJsonObject c = cv.toObject();
                        if (c["commentType"].toString() == "system" || c["isDeleted"].toBool(false)) {
                            continue;
                        }
                        PrComment pc;
                        pc.threadId = thId;
                        pc.commentId = c["id"].toInt();
                        pc.author = c["author"].toObject()["displayName"].toString("Unknown");
                        pc.date = c["publishedDate"].toString();
                        pc.content = c["content"].toString().trimmed();
                        userComments.append(pc);
                        state->results[prId].commentsMap.insert(qMakePair(thId, pc.commentId), pc);
                    }

                    if (!userComments.isEmpty() && (thStatus.isEmpty() || thStatus == "active" || thStatus == "pending")) {
                        PrActiveThread at;
                        at.threadId = thId;
                        at.status = thStatus.isEmpty() ? "active" : thStatus;
                        at.lastAuthor = userComments.last().author;
                        QString snip = userComments.last().content;
                        snip.replace('\n', ' ').replace('\r', ' ');
                        at.lastSnippet = snip.left(75);
                        state->results[prId].activeThreads.append(at);
                    }
                }
            }
            checkComplete();
        });
    }
}

QList<PrEvent> AdoClient::compareSnapshots(const QMap<int, PrSnapshot>& oldSnaps, const QMap<int, PrSnapshot>& newSnaps) {
    QList<PrEvent> events;
    QString nowStr = QTime::currentTime().toString("HH:mm:ss");

    // 1. New PRs
    for (auto it = newSnaps.cbegin(); it != newSnaps.cend(); ++it) {
        if (!oldSnaps.contains(it.key())) {
            PrEvent ev;
            ev.time = nowStr;
            ev.text = QString("✨ [PR #%1] Новий активний PR: %2").arg(it.key()).arg(it.value().title);
            ev.notifyTitle = QString("Новий PR #%1").arg(it.key());
            ev.notifyBody = it.value().title;
            events.append(ev);
        }
    }

    // 2. Closed PRs
    for (auto it = oldSnaps.cbegin(); it != oldSnaps.cend(); ++it) {
        if (!newSnaps.contains(it.key())) {
            PrEvent ev;
            ev.time = nowStr;
            ev.text = QString("🏁 [PR #%1] PR більше не активний (завершено або скасовано)").arg(it.key());
            ev.notifyTitle = QString("PR #%1 закрито").arg(it.key());
            ev.notifyBody = it.value().title;
            events.append(ev);
        }
    }

    // 3. Changes inside existing PRs
    for (auto it = newSnaps.cbegin(); it != newSnaps.cend(); ++it) {
        int prId = it.key();
        if (!oldSnaps.contains(prId)) continue;

        const PrSnapshot& oldPr = oldSnaps[prId];
        const PrSnapshot& newPr = it.value();

        // A) Builds
        for (auto bIt = newPr.builds.cbegin(); bIt != newPr.builds.cend(); ++bIt) {
            QString bName = bIt.key();
            QString newSt = bIt.value();
            QString oldSt = oldPr.builds.value(bName);
            if (oldSt != newSt) {
                QString stLower = newSt.toLower();
                if (stLower == "rejected" || stLower == "failed" || stLower == "broken") {
                    events.append({nowStr,
                                   QString("🚨 [PR #%1] Білд '%2' ВПАВ (%3)!").arg(prId).arg(bName, newSt),
                                   QString("Білд впав! (PR #%1)").arg(prId),
                                   QString("%1: %2").arg(bName, newSt),
                                   true});
                } else if (stLower == "approved" || stLower == "succeeded") {
                    events.append({nowStr,
                                   QString("✅ [PR #%1] Білд '%2' успішно пройдено!").arg(prId).arg(bName),
                                   QString("Білд пройдено (PR #%1)").arg(prId),
                                   QString("%1: %2").arg(bName, newSt),
                                   false});
                } else if (stLower == "running") {
                    events.append({nowStr,
                                   QString("⚙️ [PR #%1] Білд '%2' запущено.").arg(prId).arg(bName),
                                   QString(), QString(), false});
                }
            }
        }

        // B) New comments
        for (auto cIt = newPr.commentsMap.cbegin(); cIt != newPr.commentsMap.cend(); ++cIt) {
            if (!oldPr.commentsMap.contains(cIt.key())) {
                QString snip = cIt.value().content;
                snip.replace('\n', ' ').replace('\r', ' ');
                snip = snip.left(80);
                events.append({nowStr,
                               QString("💬 [PR #%1] Новий коментар від %2: \"%3...\"").arg(prId).arg(cIt.value().author, snip),
                               QString("Новий коментар у PR #%1").arg(prId),
                               QString("%1: %2").arg(cIt.value().author, snip),
                               false});
            }
        }

        // C) Reviewer votes
        for (auto rIt = newPr.reviewers.cbegin(); rIt != newPr.reviewers.cend(); ++rIt) {
            int oldVote = oldPr.reviewers.value(rIt.key(), 0);
            int newVote = rIt.value();
            if (newVote != oldVote) {
                QString voteStr = (newVote == 10) ? "Approved (+10)"
                                : (newVote == 5)  ? "Suggestions (+5)"
                                : (newVote == -5) ? "Waiting (-5)"
                                : (newVote == -10)? "Rejected (-10)"
                                                  : QString("Vote: %1").arg(newVote);
                events.append({nowStr,
                               QString("🗳️ [PR #%1] Рев'ювер %2 змінив голос: %3").arg(prId).arg(rIt.key(), voteStr),
                               QString("Голос у PR #%1").arg(prId),
                               QString("%1: %2").arg(rIt.key(), voteStr),
                               newVote == -10});
            }
        }
    }

    return events;
}

// ================= 3. CREATE PR (create-pr.py replacement) =================

bool AdoClient::inspectWorkspaceForPr(const QString& workspacePath, QString& outBranch, QString& outDefaultTitle, int& outTaskId, QString& outError) {
    Settings cfg = Config::loadSettings();
    QDir wsDir(workspacePath);
    QDir repoDir(wsDir.filePath(cfg.repositoryId));

    if (!repoDir.exists() || !repoDir.exists(".git")) {
        outError = QString("Підпапку Git-репозиторію не знайдено: %1").arg(QDir::toNativeSeparators(repoDir.absolutePath()));
        return false;
    }

    // 1. Current branch
    QProcess gitBranch;
    gitBranch.setWorkingDirectory(repoDir.absolutePath());
    gitBranch.start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
    gitBranch.waitForFinished(5000);
    outBranch = QString::fromUtf8(gitBranch.readAllStandardOutput()).trimmed();
    if (gitBranch.exitCode() != 0 || outBranch.isEmpty()) {
        outError = "Не вдалося визначити поточну гілку git.";
        return false;
    }
    if (outBranch == "master" || outBranch == "main" || outBranch == "HEAD") {
        outError = QString("Ви знаходитесь на службовій гілці '%1'. Перемкніться на робочу гілку задачі.").arg(outBranch);
        return false;
    }

    // 2. Last commit subject
    QProcess gitLog;
    gitLog.setWorkingDirectory(repoDir.absolutePath());
    gitLog.start("git", {"log", "-1", "--pretty=%s"});
    gitLog.waitForFinished(5000);
    QString commitSubj = QString::fromUtf8(gitLog.readAllStandardOutput()).trimmed();

    // 3. Parse <id>_*.md in workspace root
    outTaskId = 0;
    QString mdTitle;
    QFileInfoList mdFiles = wsDir.entryInfoList(QStringList() << "*_*.md", QDir::Files);
    QRegularExpression reTitle(QStringLiteral("^#\\s*\\[(\\w+)\\s*#(\\d+)\\]\\s*(.*)$"), QRegularExpression::MultilineOption);
    for (const QFileInfo& fi : mdFiles) {
        QFile f(fi.absoluteFilePath());
        if (f.open(QIODevice::ReadOnly)) {
            QString content = QString::fromUtf8(f.readAll());
            QRegularExpressionMatch m = reTitle.match(content);
            if (m.hasMatch()) {
                outTaskId = m.captured(2).toInt();
                mdTitle = QString("[%1 #%2] %3").arg(m.captured(1), m.captured(2), m.captured(3).trimmed());
                break;
            }
        }
    }

    outDefaultTitle = !commitSubj.isEmpty() ? commitSubj : (!mdTitle.isEmpty() ? mdTitle : QString("Fix for %1").arg(outBranch));
    return true;
}

void AdoClient::createPullRequest(const QString& workspacePath, const QString& prTitle) {
    QString pat = Config::getAdoPat();
    if (pat.isEmpty()) {
        emit createPrFinished(false, QString(), "Змінна середовища ADO_PAT не встановлена.");
        return;
    }

    QString branch, defaultTitle, err;
    int taskId = 0;
    if (!inspectWorkspaceForPr(workspacePath, branch, defaultTitle, taskId, err)) {
        emit createPrFinished(false, QString(), err);
        return;
    }

    Settings cfg = Config::loadSettings();
    QString repoPath = QDir(workspacePath).filePath(cfg.repositoryId);
    QString finalTitle = prTitle.trimmed().isEmpty() ? defaultTitle : prTitle.trimmed();

    emit createPrLog(QString("🌿 Пуш гілки '%1' на origin...").arg(branch));

    QProcess* pushProc = new QProcess(this);
    pushProc->setWorkingDirectory(repoPath);
    pushProc->setProcessChannelMode(QProcess::MergedChannels);

    connect(pushProc, &QProcess::finished, this, [this, pushProc, cfg, pat, branch, finalTitle, taskId](int exitCode, QProcess::ExitStatus) {
        QString output = QString::fromUtf8(pushProc->readAllStandardOutput()).trimmed();
        pushProc->deleteLater();

        if (!output.isEmpty()) {
            emit createPrLog(output);
        }
        if (exitCode != 0) {
            emit createPrFinished(false, QString(), QString("Помилка git push (код %1)").arg(exitCode));
            return;
        }

        emit createPrLog(QString("🚀 Створення Pull Request: '%1' -> '%2'...").arg(finalTitle, cfg.targetBranch));

        QString url = QString("https://dev.azure.com/%1/%2/_apis/git/repositories/%3/pullrequests?api-version=7.0")
                          .arg(cfg.organization, cfg.project, cfg.repositoryId);

        QJsonObject payload;
        payload["sourceRefName"] = QString("refs/heads/%1").arg(branch);
        payload["targetRefName"] = QString("refs/heads/%1").arg(cfg.targetBranch);
        payload["title"] = finalTitle;
        payload["description"] = taskId > 0
            ? QString("Fix/Work for Work Item #%1").arg(taskId)
            : QString("Changes from branch %1").arg(branch);

        if (taskId > 0) {
            QJsonObject wiRef;
            wiRef["id"] = QString::number(taskId);
            wiRef["url"] = QString("https://dev.azure.com/%1/%2/_apis/wit/workItems/%3")
                               .arg(cfg.organization, cfg.project)
                               .arg(taskId);
            payload["workItemRefs"] = QJsonArray{wiRef};
        }

        QNetworkReply* reply = m_nam->post(makeAdoRequest(url, pat), QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, cfg]() {
            reply->deleteLater();
            int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

            if (httpStatus == 409) {
                emit createPrFinished(false, QString(), "PR для цієї гілки вже існує в Azure DevOps!");
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                QString msg = doc.object()["message"].toString(reply->errorString());
                emit createPrFinished(false, QString(), "Помилка створення PR: " + msg);
                return;
            }

            int prId = doc.object()["pullRequestId"].toInt();
            QString prWebUrl = QString("https://dev.azure.com/%1/%2/_git/%3/pullrequest/%4")
                                   .arg(cfg.organization, cfg.project, cfg.repositoryId)
                                   .arg(prId);

            emit createPrFinished(true, prWebUrl, QString("Pull Request #%1 успішно створено!").arg(prId));
        });
    });

    pushProc->start("git", {"push", "-u", "origin", branch});
}
