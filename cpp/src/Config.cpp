#include "Config.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>

QDir Config::baseDir() {
    // Check candidate directories relative to executable:
    // e.g. C:/Users/DGalka/Automation/cpp/release -> ../.. is C:/Users/DGalka/Automation
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath("../.."),
        QDir(appDir).filePath(".."),
        appDir,
        QDir::home().filePath("Automation")
    };

    for (const QString& cand : candidates) {
        QDir dir(QDir::cleanPath(cand));
        if (dir.exists("data/settings.json") || dir.exists("settings.example.json") || dir.exists("sprint-tasks")) {
            return dir;
        }
    }
    return QDir(QDir::cleanPath(QDir(appDir).filePath("../..")));
}

QDir Config::dataDir() {
    return QDir(baseDir().filePath("data"));
}

QDir Config::sprintTasksDir() {
    return QDir(baseDir().filePath("sprint-tasks"));
}

QString Config::settingsFilePath() {
    return dataDir().filePath("settings.json");
}

QString Config::linksFilePath() {
    return dataDir().filePath("links.json");
}

QString Config::iconFilePath() {
    return baseDir().filePath("icon.ico");
}

Settings Config::loadSettings() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    Settings s;
    s.organization = env.value("ADO_ORG", "your-org");
    s.project = env.value("ADO_PROJECT", "YourProject");
    s.repositoryId = env.value("ADO_REPO", "YourRepo");
    s.targetBranch = "master";
    s.queryId = env.value("ADO_QUERY_ID", "");
    s.userPrefix = env.value("USER_PREFIX", env.value("USERNAME", "user").toLower());
    s.branchPrefix = "feature";
    s.gitRoots = {
        QDir::toNativeSeparators(QDir::home().filePath("Git")),
        QStringLiteral("D:\\Git")
    };
    s.clangFormatPath = "clang-format.exe";
    s.prWatchInterval = 60;
    s.prWatchNotify = false;
    s.prWatchSound = true;
    s.prWatchIncludeDrafts = false;

    QFile file(settingsFilePath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("organization")) s.organization = obj["organization"].toString(s.organization);
            if (obj.contains("project")) s.project = obj["project"].toString(s.project);
            if (obj.contains("repository_id")) s.repositoryId = obj["repository_id"].toString(s.repositoryId);
            if (obj.contains("target_branch")) s.targetBranch = obj["target_branch"].toString(s.targetBranch);
            if (obj.contains("query_id")) s.queryId = obj["query_id"].toString(s.queryId);
            if (obj.contains("user_prefix")) s.userPrefix = obj["user_prefix"].toString(s.userPrefix);
            if (obj.contains("branch_prefix")) s.branchPrefix = obj["branch_prefix"].toString(s.branchPrefix);
            if (obj.contains("clang_format_path")) s.clangFormatPath = obj["clang_format_path"].toString(s.clangFormatPath);
            if (obj.contains("pr_watch_interval")) s.prWatchInterval = obj["pr_watch_interval"].toInt(s.prWatchInterval);
            if (obj.contains("pr_watch_notify")) s.prWatchNotify = obj["pr_watch_notify"].toBool(s.prWatchNotify);
            if (obj.contains("pr_watch_sound")) s.prWatchSound = obj["pr_watch_sound"].toBool(s.prWatchSound);
            if (obj.contains("pr_watch_include_drafts")) s.prWatchIncludeDrafts = obj["pr_watch_include_drafts"].toBool(s.prWatchIncludeDrafts);

            if (obj.contains("git_roots") && obj["git_roots"].isArray()) {
                QStringList roots;
                for (const QJsonValue& v : obj["git_roots"].toArray()) {
                    QString r = v.toString().trimmed();
                    if (!r.isEmpty()) roots.append(r);
                }
                if (!roots.isEmpty()) {
                    s.gitRoots = roots;
                }
            }
        }
    }
    return s;
}

bool Config::saveSettings(const Settings& s) {
    QDir d = dataDir();
    if (!d.exists()) {
        d.mkpath(".");
    }

    QJsonObject obj;
    obj["organization"] = s.organization;
    obj["project"] = s.project;
    obj["repository_id"] = s.repositoryId;
    obj["target_branch"] = s.targetBranch;
    obj["query_id"] = s.queryId;
    obj["user_prefix"] = s.userPrefix;
    obj["branch_prefix"] = s.branchPrefix;

    QJsonArray rootsArr;
    for (const QString& r : s.gitRoots) {
        if (!r.trimmed().isEmpty()) {
            rootsArr.append(r.trimmed());
        }
    }
    obj["git_roots"] = rootsArr;
    obj["clang_format_path"] = s.clangFormatPath;
    obj["pr_watch_interval"] = s.prWatchInterval;
    obj["pr_watch_notify"] = s.prWatchNotify;
    obj["pr_watch_sound"] = s.prWatchSound;
    obj["pr_watch_include_drafts"] = s.prWatchIncludeDrafts;

    QFile file(settingsFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

QList<QDir> Config::getGitRoots(const Settings& s) {
    QList<QDir> res;
    for (const QString& r : s.gitRoots) {
        if (!r.trimmed().isEmpty()) {
            res.append(QDir(r.trimmed()));
        }
    }
    return res;
}

QList<QuickLink> Config::loadLinks() {
    QList<QuickLink> list;
    QFile file(linksFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return list;
    }
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        return list;
    }
    for (const QJsonValue& val : doc.array()) {
        if (!val.isObject()) continue;
        QJsonObject o = val.toObject();
        QuickLink item;
        item.title = o["title"].toString();
        item.category = o["category"].toString("Інше");
        item.url = o["url"].toString();
        item.icon = o["icon"].toString("link");
        item.description = o["description"].toString();
        list.append(item);
    }
    return list;
}

bool Config::saveLinks(const QList<QuickLink>& links) {
    QDir d = dataDir();
    if (!d.exists()) {
        d.mkpath(".");
    }

    QJsonArray arr;
    for (const QuickLink& l : links) {
        QJsonObject o;
        o["title"] = l.title;
        o["category"] = l.category;
        o["url"] = l.url;
        o["icon"] = l.icon;
        o["description"] = l.description;
        arr.append(o);
    }

    QFile file(linksFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

QString Config::getAdoPat() {
    return QProcessEnvironment::systemEnvironment().value("ADO_PAT").trimmed();
}
