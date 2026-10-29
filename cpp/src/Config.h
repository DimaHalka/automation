#pragma once

#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>

struct Settings {
    QString organization;
    QString project;
    QString repositoryId;
    QString targetBranch = "master";
    QString queryId;
    QString userPrefix;
    QString branchPrefix = "feature";
    QStringList gitRoots;
    QString clangFormatPath = "clang-format.exe";
    int prWatchInterval = 60;
    bool prWatchNotify = false;
    bool prWatchSound = true;
    bool prWatchIncludeDrafts = false;
};

struct QuickLink {
    QString title;
    QString category = "Azure DevOps";
    QString url;
    QString icon = "link";
    QString description;
};

class Config {
public:
    static QDir baseDir();
    static QDir dataDir();
    static QDir sprintTasksDir();
    static QString settingsFilePath();
    static QString linksFilePath();
    static QString iconFilePath();

    static Settings loadSettings();
    static bool saveSettings(const Settings& s);

    static QList<QDir> getGitRoots(const Settings& s);
    static QList<QuickLink> loadLinks();
    static bool saveLinks(const QList<QuickLink>& links);

    static QString getAdoPat();
};
