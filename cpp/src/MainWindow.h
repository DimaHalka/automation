#pragma once

#include "AdoClient.h"
#include "Config.h"
#include "WorkspaceManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void applyDarkWindowFrame();
    void setupUi();
    void switchTab(int index);
    void jumpToSetup(const SprintTask& task);
    void refreshDrivesWidget();
    void showStatusToast(const QString& message, bool isError = false);

    // Pages construction
    QWidget* createSprintPage();
    QWidget* createSetupPage();
    QWidget* createPrPage();
    QWidget* createSpacePage();
    QWidget* createLinksSettingsPage();

    // Dynamic refreshers
    void refreshSprintTasksList();
    void populateSprintCards();
    void updateSetupPreview();
    void refreshPrView();
    void refreshSpaceView();
    void refreshLinksView();

    // Dialogs
    void openCreatePrDialog(const QString& workspacePath);
    void showTaskMarkdownDialog(const SprintTask& task);

    // Core services
    AdoClient* m_ado = nullptr;
    TaskSetupRunner* m_setupRunner = nullptr;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QTimer* m_prTimer = nullptr;

    // Navigation & Header
    QMap<int, QPushButton*> m_navButtons;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_prHeaderLabel = nullptr;
    QVBoxLayout* m_drivesLayout = nullptr;
    QLabel* m_statusToastLabel = nullptr;

    // Page 0: Sprint Tasks state & widgets
    QList<SprintTask> m_sprintTasks;
    QString m_sprintStatusFilter = "Всі";
    QString m_sprintSearchQuery;
    QWidget* m_sprintFilterRow = nullptr;
    QVBoxLayout* m_sprintCardsLayout = nullptr;
    QLabel* m_sprintSubtitleLabel = nullptr;
    QLabel* m_sprintSyncStatusLabel = nullptr;
    QWidget* m_sprintLogBox = nullptr;
    QPlainTextEdit* m_sprintSyncLog = nullptr;
    QPushButton* m_sprintSyncBtn = nullptr;

    // Page 1: Task Setup widgets
    QComboBox* m_setupMdCombo = nullptr;
    QLineEdit* m_setupTaskInput = nullptr;
    QLabel* m_setupFolderPreview = nullptr;
    QLabel* m_setupBranchPreview = nullptr;
    QLabel* m_setupStatusLabel = nullptr;
    QPlainTextEdit* m_setupTerminal = nullptr;
    QPushButton* m_setupRunBtn = nullptr;

    // Page 2: PR Monitor state & widgets
    bool m_prMonitoringActive = true;
    bool m_prNotifyEnabled = false;
    bool m_prSoundEnabled = true;
    bool m_prIncludeDrafts = false;
    bool m_hasInitialPrSnapshot = false;
    QString m_prLastCheck = "Ще не перевірялося";
    QString m_prError;
    QMap<int, PrSnapshot> m_prSnapshots;
    QList<PrEvent> m_prEvents;
    QLabel* m_prSubtitleLabel = nullptr;
    QVBoxLayout* m_prCardsLayout = nullptr;
    QVBoxLayout* m_prEventsLayout = nullptr;

    // Page 3: Disk Usage (Keep Space) state & widgets
    QList<WorkspaceFolder> m_workspaces;
    QSet<QString> m_selectedWorkspaces;
    bool m_isDeletingWorkspaces = false;
    QLabel* m_spaceTotalLabel = nullptr;
    QLabel* m_spaceClosedLabel = nullptr;
    QLabel* m_spaceSelectedLabel = nullptr;
    QVBoxLayout* m_spaceListLayout = nullptr;

    // Page 4: Links & Settings widgets
    QVBoxLayout* m_linksGridLayout = nullptr;
    QLineEdit* m_cfgOrg = nullptr;
    QLineEdit* m_cfgProj = nullptr;
    QLineEdit* m_cfgRepo = nullptr;
    QLineEdit* m_cfgQuery = nullptr;
    QLineEdit* m_cfgTarget = nullptr;
    QLineEdit* m_cfgBranchPref = nullptr;
    QLineEdit* m_cfgUserPref = nullptr;
    QSpinBox* m_cfgPrInterval = nullptr;
    QLineEdit* m_cfgGitRoots = nullptr;
    QLineEdit* m_cfgClang = nullptr;
};
