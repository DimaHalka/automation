#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

static void clearLayout(QLayout* layout) {
    if (!layout) return;
    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        if (QLayout* childLayout = item->layout()) {
            clearLayout(childLayout);
        }
        delete item;
    }
}

static QString stateBadgeStyle(const QString& state) {
    QString s = state.trimmed().toLower();
    if (s == "active") {
        return "background-color: #1d4ed8; color: #eff6ff; border-radius: 4px; padding: 2px 8px; font-size: 11px; font-weight: 600;";
    }
    if (s == "proposed" || s == "new") {
        return "background-color: #b45309; color: #fffbeb; border-radius: 4px; padding: 2px 8px; font-size: 11px; font-weight: 600;";
    }
    if (s == "resolved" || s == "closed" || s == "done" || s == "completed") {
        return "background-color: #047857; color: #ecfdf5; border-radius: 4px; padding: 2px 8px; font-size: 11px; font-weight: 600;";
    }
    return "background-color: #334155; color: #e2e8f0; border-radius: 4px; padding: 2px 8px; font-size: 11px; font-weight: 600;";
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Automation Hub");
    resize(1440, 900);

    QString iconPath = Config::iconFilePath();
    if (QFileInfo::exists(iconPath)) {
        setWindowIcon(QIcon(iconPath));
    }

    Settings cfg = Config::loadSettings();
    m_prNotifyEnabled = cfg.prWatchNotify;
    m_prSoundEnabled = cfg.prWatchSound;
    m_prIncludeDrafts = cfg.prWatchIncludeDrafts;

    m_ado = new AdoClient(this);
    m_setupRunner = new TaskSetupRunner(this);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_trayIcon = new QSystemTrayIcon(windowIcon(), this);
        m_trayIcon->setToolTip("Automation Hub — PR Monitor");
        m_trayIcon->show();
    }

    setupUi();
    applyDarkWindowFrame();

    // Connect Sprint Sync signals
    connect(m_ado, &AdoClient::sprintSyncLog, this, [this](const QString& line) {
        if (m_sprintSyncLog) {
            m_sprintSyncLog->appendPlainText(line);
        }
    });
    connect(m_ado, &AdoClient::sprintSyncFinished, this, [this](bool ok, const QString& msg) {
        if (m_sprintSyncBtn) m_sprintSyncBtn->setEnabled(true);
        if (m_sprintSyncStatusLabel) {
            m_sprintSyncStatusLabel->setText(ok ? "✅ Готово" : "❌ Помилка");
        }
        showStatusToast(msg, !ok);
        refreshSprintTasksList();
        refreshSpaceView();
    });

    // Connect Task Setup signals
    connect(m_setupRunner, &TaskSetupRunner::logLine, this, [this](const QString& line) {
        if (m_setupTerminal) {
            m_setupTerminal->appendPlainText(line);
            m_setupTerminal->verticalScrollBar()->setValue(m_setupTerminal->verticalScrollBar()->maximum());
        }
    });
    connect(m_setupRunner, &TaskSetupRunner::statusChanged, this, [this](const QString& st) {
        if (m_setupStatusLabel) m_setupStatusLabel->setText(st);
    });
    connect(m_setupRunner, &TaskSetupRunner::finished, this, [this](bool ok, const QString& msg) {
        if (m_setupRunBtn) m_setupRunBtn->setEnabled(true);
        showStatusToast(msg, !ok);
        refreshDrivesWidget();
        refreshSprintTasksList();
        refreshSpaceView();
    });

    // Connect PR Monitor signals
    connect(m_ado, &AdoClient::prSnapshotsReady, this, [this](const QMap<int, PrSnapshot>& newSnaps) {
        QString nowStr = QTime::currentTime().toString("HH:mm:ss");
        if (m_hasInitialPrSnapshot) {
            QList<PrEvent> diff = AdoClient::compareSnapshots(m_prSnapshots, newSnaps);
            for (const PrEvent& ev : diff) {
                m_prEvents.prepend(ev);
                if (m_prNotifyEnabled && m_trayIcon && !ev.notifyTitle.isEmpty()) {
                    m_trayIcon->showMessage(ev.notifyTitle, ev.notifyBody,
                                            ev.isError ? QSystemTrayIcon::Critical : QSystemTrayIcon::Information, 6000);
                }
#ifdef Q_OS_WIN
                if (m_prSoundEnabled) {
                    MessageBeep(ev.isError ? MB_ICONHAND : MB_ICONASTERISK);
                }
#endif
            }
            while (m_prEvents.size() > 50) {
                m_prEvents.removeLast();
            }
        }
        m_hasInitialPrSnapshot = true;
        m_prSnapshots = newSnaps;
        m_prLastCheck = nowStr;
        m_prError.clear();

        if (m_prHeaderLabel) {
            m_prHeaderLabel->setText(QString("📡 PRs: %1 активних (%2)").arg(newSnaps.size()).arg(nowStr));
        }
        refreshPrView();
    });

    connect(m_ado, &AdoClient::prFetchError, this, [this](const QString& err) {
        m_prError = err;
        if (m_prHeaderLabel) {
            m_prHeaderLabel->setText("📡 PR Monitor: помилка");
        }
        refreshPrView();
    });

    // Background PR timer
    m_prTimer = new QTimer(this);
    connect(m_prTimer, &QTimer::timeout, this, [this]() {
        if (!m_prMonitoringActive) {
            if (m_prHeaderLabel) m_prHeaderLabel->setText("📡 PR Monitor: пауза");
            return;
        }
        m_ado->fetchPullRequests(m_prIncludeDrafts);
    });
    m_prTimer->start(qMax(15, cfg.prWatchInterval) * 1000);

    // Initial PR fetch 1.5s after startup
    QTimer::singleShot(1500, this, [this]() {
        if (m_prMonitoringActive && !Config::getAdoPat().isEmpty()) {
            m_ado->fetchPullRequests(m_prIncludeDrafts);
        }
    });
}

void MainWindow::applyDarkWindowFrame() {
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL useDark = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20
    DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
#endif
}

void MainWindow::showStatusToast(const QString& message, bool isError) {
    if (!m_statusToastLabel) return;
    m_statusToastLabel->setText(message);
    m_statusToastLabel->setStyleSheet(isError
        ? "background-color: #7f1d1d; color: #fecaca; border: 1px solid #ef4444; border-radius: 12px; padding: 4px 12px; font-size: 12px; font-weight: 600;"
        : "background-color: #064e3b; color: #a7f3d0; border: 1px solid #10b981; border-radius: 12px; padding: 4px 12px; font-size: 12px; font-weight: 600;");
    m_statusToastLabel->show();
    QTimer::singleShot(4500, m_statusToastLabel, &QWidget::hide);
}

void MainWindow::setupUi() {
    QWidget* root = new QWidget(this);
    setCentralWidget(root);

    QVBoxLayout* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ================= HEADER =================
    QFrame* header = new QFrame(root);
    header->setObjectName("HeaderBar");
    header->setFixedHeight(56);
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, 0, 20, 0);
    headerLayout->setSpacing(14);

    QLabel* logoIcon = new QLabel("⚡", header);
    logoIcon->setStyleSheet("font-size: 20px; color: #60a5fa;");
    headerLayout->addWidget(logoIcon);

    QLabel* titleLbl = new QLabel("Automation Hub", header);
    titleLbl->setStyleSheet("font-size: 17px; font-weight: 700; color: #ffffff;");
    headerLayout->addWidget(titleLbl);

    QLabel* nativeBadge = new QLabel("C++20 • Qt 6", header);
    nativeBadge->setStyleSheet("background-color: #172554; color: #93c5fd; border: 1px solid #1e40af; border-radius: 10px; padding: 2px 8px; font-size: 11px; font-family: 'Cascadia Mono', 'Consolas', monospace;");
    headerLayout->addWidget(nativeBadge);

    headerLayout->addStretch();

    m_statusToastLabel = new QLabel(header);
    m_statusToastLabel->hide();
    headerLayout->addWidget(m_statusToastLabel);

    // ADO_PAT pill
    bool hasPat = !Config::getAdoPat().isEmpty();
    QLabel* patBadge = new QLabel(hasPat ? "🛡️ ADO_PAT: OK" : "⚠️ ADO_PAT: Відсутній!", header);
    patBadge->setStyleSheet(QString(
        "background-color: #141824; border: 1px solid #1e2536; border-radius: 12px; padding: 4px 12px; "
        "font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: %1;")
        .arg(hasPat ? "#34d399" : "#f87171"));
    headerLayout->addWidget(patBadge);

    // PR Monitor pill
    QPushButton* prPillBtn = new QPushButton(header);
    prPillBtn->setCursor(Qt::PointingHandCursor);
    prPillBtn->setFlat(true);
    prPillBtn->setStyleSheet("QPushButton { background-color: #141824; border: 1px solid #1e2536; border-radius: 12px; padding: 4px 12px; } QPushButton:hover { border-color: #3b82f6; }");
    QHBoxLayout* prPillLayout = new QHBoxLayout(prPillBtn);
    prPillLayout->setContentsMargins(8, 2, 8, 2);
    m_prHeaderLabel = new QLabel("📡 PR Monitor: активний", prPillBtn);
    m_prHeaderLabel->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #cbd5e1; background: transparent; border: none;");
    prPillLayout->addWidget(m_prHeaderLabel);
    connect(prPillBtn, &QPushButton::clicked, this, [this]() { switchTab(2); });
    headerLayout->addWidget(prPillBtn);

    rootLayout->addWidget(header);

    // ================= BODY: SIDEBAR + STACK =================
    QWidget* body = new QWidget(root);
    QHBoxLayout* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    // Left Sidebar
    QFrame* sidebar = new QFrame(body);
    sidebar->setObjectName("Sidebar");
    sidebar->setFixedWidth(255);
    QVBoxLayout* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 16, 14, 16);
    sideLayout->setSpacing(6);

    QLabel* wsSectionLbl = new QLabel("РОБОЧИЙ ПРОСТІР", sidebar);
    wsSectionLbl->setStyleSheet("font-size: 11px; font-weight: 700; color: #64748b; padding-left: 8px; margin-bottom: 4px; letter-spacing: 0.8px;");
    sideLayout->addWidget(wsSectionLbl);

    struct NavItem {
        int index;
        QString label;
    };
    const QList<NavItem> navItems = {
        {0, "📋  Таски спринту"},
        {1, "💻  Робоче оточення"},
        {2, "🔀  Pull Requests"},
        {3, "🧹  Disk Usage"},
        {4, "🔖  Лінки та Налаштування"}
    };

    for (const NavItem& item : navItems) {
        QPushButton* btn = new QPushButton(item.label, sidebar);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("navActive", item.index == 0);
        btn->setObjectName("NavButton");
        connect(btn, &QPushButton::clicked, this, [this, idx = item.index]() { switchTab(idx); });
        sideLayout->addWidget(btn);
        m_navButtons.insert(item.index, btn);
    }

    sideLayout->addStretch();

    // Bottom Drives widget
    QFrame* drivesDivider = new QFrame(sidebar);
    drivesDivider->setFrameShape(QFrame::HLine);
    drivesDivider->setStyleSheet("background-color: #1e2433; max-height: 1px; border: none;");
    sideLayout->addWidget(drivesDivider);

    QLabel* drivesHeaderLbl = new QLabel("ДИСКИ (GIT ROOTS)", sidebar);
    drivesHeaderLbl->setStyleSheet("font-size: 11px; font-weight: 700; color: #64748b; padding-left: 4px; margin-top: 8px; margin-bottom: 4px; letter-spacing: 0.8px;");
    sideLayout->addWidget(drivesHeaderLbl);

    QWidget* drivesContainer = new QWidget(sidebar);
    m_drivesLayout = new QVBoxLayout(drivesContainer);
    m_drivesLayout->setContentsMargins(0, 0, 0, 0);
    m_drivesLayout->setSpacing(8);
    sideLayout->addWidget(drivesContainer);

    refreshDrivesWidget();
    bodyLayout->addWidget(sidebar);

    // Main Stacked Content
    m_stack = new QStackedWidget(body);
    m_stack->addWidget(createSprintPage());
    m_stack->addWidget(createSetupPage());
    m_stack->addWidget(createPrPage());
    m_stack->addWidget(createSpacePage());
    m_stack->addWidget(createLinksSettingsPage());

    bodyLayout->addWidget(m_stack, 1);
    rootLayout->addWidget(body, 1);
}

void MainWindow::switchTab(int index) {
    if (!m_stack || index < 0 || index >= m_stack->count()) return;
    m_stack->setCurrentIndex(index);

    for (auto it = m_navButtons.begin(); it != m_navButtons.end(); ++it) {
        bool active = (it.key() == index);
        it.value()->setProperty("navActive", active);
        it.value()->style()->unpolish(it.value());
        it.value()->style()->polish(it.value());
    }

    refreshDrivesWidget();
    if (index == 0) {
        refreshSprintTasksList();
    } else if (index == 1) {
        updateSetupPreview();
    } else if (index == 2) {
        refreshPrView();
    } else if (index == 3) {
        refreshSpaceView();
    }
}

void MainWindow::jumpToSetup(const SprintTask& task) {
    // Refresh combo items first so task.mdPath is guaranteed to exist
    if (m_setupMdCombo) {
        int idx = m_setupMdCombo->findData(task.mdPath);
        if (idx >= 0) {
            m_setupMdCombo->setCurrentIndex(idx);
        }
    }
    if (m_setupTaskInput) {
        m_setupTaskInput->setText(WorkspaceManager::suggestShortTaskName(task.id, task.title));
    }
    switchTab(1);
}

void MainWindow::refreshDrivesWidget() {
    if (!m_drivesLayout) return;
    clearLayout(m_drivesLayout);

    for (const DriveInfo& d : WorkspaceManager::getDrivesInfo()) {
        QFrame* card = new QFrame();
        card->setObjectName("DriveCard");
        QVBoxLayout* l = new QVBoxLayout(card);
        l->setContentsMargins(10, 8, 10, 8);
        l->setSpacing(5);

        QHBoxLayout* topRow = new QHBoxLayout();
        QLabel* rootLbl = new QLabel(QString("%1 (%2)").arg(d.drive, d.root));
        rootLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 11px; color: #cbd5e1;");
        QLabel* freeLbl = new QLabel(QString("%1 GB вільно").arg(d.freeGb, 0, 'f', 1));
        freeLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 11px; color: #60a5fa; font-weight: 600;");
        topRow->addWidget(rootLbl);
        topRow->addStretch();
        topRow->addWidget(freeLbl);
        l->addLayout(topRow);

        QProgressBar* bar = new QProgressBar();
        bar->setRange(0, 100);
        bar->setValue(static_cast<int>(d.usedPct));
        bar->setTextVisible(false);
        bar->setFixedHeight(6);
        bar->setStyleSheet(
            "QProgressBar { background-color: #1e293b; border-radius: 3px; border: none; }"
            "QProgressBar::chunk { background-color: #3b82f6; border-radius: 3px; }");
        l->addWidget(bar);

        m_drivesLayout->addWidget(card);
    }
}

// ================= PAGE 0: SPRINT TASKS =================

QWidget* MainWindow::createSprintPage() {
    QWidget* page = new QWidget();
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(14);

    // Top row
    QHBoxLayout* topRow = new QHBoxLayout();
    QVBoxLayout* titleCol = new QVBoxLayout();
    titleCol->setSpacing(2);
    QLabel* h1 = new QLabel("Таски на спринті");
    h1->setStyleSheet("font-size: 22px; font-weight: 700; color: #ffffff;");
    m_sprintSubtitleLabel = new QLabel("Локальний кеш у папці sprint-tasks/");
    m_sprintSubtitleLabel->setStyleSheet("font-size: 13px; color: #94a3b8;");
    titleCol->addWidget(h1);
    titleCol->addWidget(m_sprintSubtitleLabel);
    topRow->addLayout(titleCol);

    topRow->addStretch();

    m_sprintSyncStatusLabel = new QLabel("");
    m_sprintSyncStatusLabel->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #94a3b8;");
    topRow->addWidget(m_sprintSyncStatusLabel);

    QPushButton* toggleLogBtn = new QPushButton("📄 Лог");
    toggleLogBtn->setObjectName("OutlineButton");
    toggleLogBtn->setCursor(Qt::PointingHandCursor);
    topRow->addWidget(toggleLogBtn);

    m_sprintSyncBtn = new QPushButton("🔄 Sync Sprint з ADO");
    m_sprintSyncBtn->setObjectName("PrimaryButton");
    m_sprintSyncBtn->setCursor(Qt::PointingHandCursor);
    topRow->addWidget(m_sprintSyncBtn);

    pageLayout->addLayout(topRow);

    // Collapsible Sync Log box
    m_sprintLogBox = new QFrame();
    m_sprintLogBox->setObjectName("AgyCard");
    QVBoxLayout* logBoxLayout = new QVBoxLayout(m_sprintLogBox);
    logBoxLayout->setContentsMargins(12, 10, 12, 10);
    m_sprintSyncLog = new QPlainTextEdit();
    m_sprintSyncLog->setReadOnly(true);
    m_sprintSyncLog->setFixedHeight(140);
    m_sprintSyncLog->setObjectName("TerminalEdit");
    logBoxLayout->addWidget(m_sprintSyncLog);
    m_sprintLogBox->hide();
    pageLayout->addWidget(m_sprintLogBox);

    connect(toggleLogBtn, &QPushButton::clicked, this, [this]() {
        m_sprintLogBox->setVisible(!m_sprintLogBox->isVisible());
    });

    connect(m_sprintSyncBtn, &QPushButton::clicked, this, [this]() {
        m_sprintLogBox->show();
        m_sprintSyncLog->clear();
        m_sprintSyncBtn->setEnabled(false);
        m_sprintSyncStatusLabel->setText("⏳ Синхронізація...");
        m_ado->syncSprintTasks();
    });

    // Filter + Search bar
    QHBoxLayout* filterBar = new QHBoxLayout();
    m_sprintFilterRow = new QWidget();
    QHBoxLayout* filterBtnsLayout = new QHBoxLayout(m_sprintFilterRow);
    filterBtnsLayout->setContentsMargins(0, 0, 0, 0);
    filterBtnsLayout->setSpacing(6);
    filterBar->addWidget(m_sprintFilterRow);

    filterBar->addStretch();

    QLineEdit* searchEdit = new QLineEdit();
    searchEdit->setPlaceholderText("🔍 Пошук за #ID або назвою...");
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setFixedWidth(290);
    connect(searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_sprintSearchQuery = text.trimmed();
        populateSprintCards();
    });
    filterBar->addWidget(searchEdit);
    pageLayout->addLayout(filterBar);

    // Scrollable Task Cards
    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget* scrollContent = new QWidget();
    m_sprintCardsLayout = new QVBoxLayout(scrollContent);
    m_sprintCardsLayout->setContentsMargins(0, 0, 6, 0);
    m_sprintCardsLayout->setSpacing(10);
    scroll->setWidget(scrollContent);
    pageLayout->addWidget(scroll, 1);

    refreshSprintTasksList();
    return page;
}

void MainWindow::refreshSprintTasksList() {
    m_sprintTasks = WorkspaceManager::loadSprintTasks();
    if (m_sprintSubtitleLabel) {
        m_sprintSubtitleLabel->setText(
            QString("Локальний кеш у папці sprint-tasks/ (%1 задач знайдено)").arg(m_sprintTasks.size()));
    }

    // Also update Task Setup combo box options
    if (m_setupMdCombo) {
        QString currentData = m_setupMdCombo->currentData().toString();
        m_setupMdCombo->blockSignals(true);
        m_setupMdCombo->clear();
        m_setupMdCombo->addItem("— Без прив'язки до .md файлу —", QString(""));
        for (const SprintTask& t : m_sprintTasks) {
            QString label = QString("[%1] #%2 — %3").arg(t.state).arg(t.id).arg(t.title.left(60));
            m_setupMdCombo->addItem(label, t.mdPath);
        }
        int idx = m_setupMdCombo->findData(currentData);
        if (idx >= 0) m_setupMdCombo->setCurrentIndex(idx);
        m_setupMdCombo->blockSignals(false);
    }

    // Rebuild filter buttons with counts
    if (m_sprintFilterRow && m_sprintFilterRow->layout()) {
        clearLayout(m_sprintFilterRow->layout());
        QMap<QString, int> counts;
        counts["Всі"] = m_sprintTasks.size();
        for (const SprintTask& t : m_sprintTasks) {
            counts[t.state]++;
        }

        const QStringList states = {"Всі", "Active", "Proposed", "New", "Resolved", "Closed"};
        for (const QString& st : states) {
            if (st != "Всі" && counts.value(st, 0) == 0) continue;
            QPushButton* b = new QPushButton(QString("%1 (%2)").arg(st).arg(counts.value(st, 0)));
            b->setCursor(Qt::PointingHandCursor);
            bool selected = (m_sprintStatusFilter == st);
            b->setObjectName(selected ? "PrimarySmallButton" : "OutlineSmallButton");
            connect(b, &QPushButton::clicked, this, [this, st]() {
                m_sprintStatusFilter = st;
                refreshSprintTasksList();
            });
            m_sprintFilterRow->layout()->addWidget(b);
        }
    }

    populateSprintCards();
}

void MainWindow::populateSprintCards() {
    if (!m_sprintCardsLayout) return;
    clearLayout(m_sprintCardsLayout);

    QString q = m_sprintSearchQuery.toLower();
    QList<SprintTask> filtered;
    for (const SprintTask& t : m_sprintTasks) {
        if (m_sprintStatusFilter != "Всі" && t.state != m_sprintStatusFilter) {
            continue;
        }
        if (!q.isEmpty()) {
            bool match = QString::number(t.id).contains(q) ||
                         t.title.toLower().contains(q) ||
                         t.type.toLower().contains(q);
            if (!match) continue;
        }
        filtered.append(t);
    }

    if (filtered.isEmpty()) {
        QFrame* emptyCard = new QFrame();
        emptyCard->setObjectName("AgyCard");
        QVBoxLayout* el = new QVBoxLayout(emptyCard);
        el->setContentsMargins(24, 32, 24, 32);
        QLabel* lbl = new QLabel("📭 Задач за цим фільтром не знайдено. Натисніть 'Sync Sprint з ADO'.");
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("color: #94a3b8; font-size: 14px;");
        el->addWidget(lbl);
        m_sprintCardsLayout->addWidget(emptyCard);
        m_sprintCardsLayout->addStretch();
        return;
    }

    for (const SprintTask& t : filtered) {
        QFrame* card = new QFrame();
        card->setObjectName("AgyCardHover");
        QHBoxLayout* cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(16, 12, 16, 12);
        cardLayout->setSpacing(12);

        // Left info column
        QVBoxLayout* infoCol = new QVBoxLayout();
        infoCol->setSpacing(4);

        QHBoxLayout* metaRow = new QHBoxLayout();
        metaRow->setSpacing(8);
        QLabel* badge = new QLabel(t.state);
        badge->setStyleSheet(stateBadgeStyle(t.state));
        metaRow->addWidget(badge);

        QLabel* idTypeLbl = new QLabel(QString("#%1 • %2").arg(t.id).arg(t.type));
        idTypeLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #94a3b8; font-weight: 600;");
        metaRow->addWidget(idTypeLbl);

        if (t.priority != "N/A" && !t.priority.isEmpty()) {
            QLabel* prioLbl = new QLabel(QString("P%1").arg(t.priority));
            prioLbl->setStyleSheet("background-color: #451a03; color: #fbbf24; border-radius: 4px; padding: 1px 6px; font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 11px;");
            metaRow->addWidget(prioLbl);
        }
        metaRow->addStretch();
        infoCol->addLayout(metaRow);

        QLabel* titleLbl = new QLabel(t.title);
        titleLbl->setWordWrap(true);
        titleLbl->setStyleSheet("font-size: 14px; font-weight: 600; color: #f1f5f9;");
        infoCol->addWidget(titleLbl);

        if (!t.folder.isEmpty()) {
            QLabel* folderLbl = new QLabel(QString("📁 %1").arg(t.folder));
            folderLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #34d399; margin-top: 2px;");
            infoCol->addWidget(folderLbl);
        }

        cardLayout->addLayout(infoCol, 1);

        // Right action buttons
        QHBoxLayout* actionsRow = new QHBoxLayout();
        actionsRow->setSpacing(6);

        QPushButton* descBtn = new QPushButton("📄 Опис");
        descBtn->setObjectName("OutlineSmallButton");
        descBtn->setCursor(Qt::PointingHandCursor);
        descBtn->setToolTip("Переглянути повний опис задачі (.md)");
        connect(descBtn, &QPushButton::clicked, this, [this, t]() { showTaskMarkdownDialog(t); });
        actionsRow->addWidget(descBtn);

        if (!t.folder.isEmpty()) {
            QPushButton* expBtn = new QPushButton("📂 Explorer");
            expBtn->setObjectName("EmeraldSmallButton");
            expBtn->setCursor(Qt::PointingHandCursor);
            connect(expBtn, &QPushButton::clicked, this, [p = t.folder]() { WorkspaceManager::openInExplorer(p); });
            actionsRow->addWidget(expBtn);

            QPushButton* codeBtn = new QPushButton("💻 IDE");
            codeBtn->setObjectName("OutlineSmallButton");
            codeBtn->setCursor(Qt::PointingHandCursor);
            codeBtn->setToolTip("Відкрити у Antigravity / VS Code");
            connect(codeBtn, &QPushButton::clicked, this, [this, p = t.folder]() {
                QString ed = WorkspaceManager::openInCodeEditor(p);
                showStatusToast(QString("Відкриваю у %1: %2").arg(ed, QFileInfo(p).fileName()));
            });
            actionsRow->addWidget(codeBtn);

            QPushButton* prBtn = new QPushButton("🔀 Create PR");
            prBtn->setObjectName("PrimarySmallButton");
            prBtn->setCursor(Qt::PointingHandCursor);
            prBtn->setToolTip("Запушити гілку та створити Pull Request у ADO");
            connect(prBtn, &QPushButton::clicked, this, [this, p = t.folder]() { openCreatePrDialog(p); });
            actionsRow->addWidget(prBtn);
        } else {
            QPushButton* setupBtn = new QPushButton("🚀 Setup Env");
            setupBtn->setObjectName("PrimarySmallButton");
            setupBtn->setCursor(Qt::PointingHandCursor);
            connect(setupBtn, &QPushButton::clicked, this, [this, t]() { jumpToSetup(t); });
            actionsRow->addWidget(setupBtn);
        }

        if (!t.url.isEmpty()) {
            QPushButton* adoBtn = new QPushButton("↗ ADO");
            adoBtn->setObjectName("OutlineSmallButton");
            adoBtn->setCursor(Qt::PointingHandCursor);
            connect(adoBtn, &QPushButton::clicked, this, [u = t.url]() { WorkspaceManager::openExternalUrl(u); });
            actionsRow->addWidget(adoBtn);
        }

        cardLayout->addLayout(actionsRow);
        m_sprintCardsLayout->addWidget(card);
    }

    m_sprintCardsLayout->addStretch();
}

void MainWindow::showTaskMarkdownDialog(const SprintTask& task) {
    QDialog dlg(this);
    dlg.setWindowTitle(QString("[%1 #%2] %3").arg(task.type).arg(task.id).arg(task.title));
    dlg.resize(760, 540);

    QVBoxLayout* l = new QVBoxLayout(&dlg);
    l->setContentsMargins(18, 18, 18, 18);
    l->setSpacing(12);

    QPlainTextEdit* viewer = new QPlainTextEdit(task.content, &dlg);
    viewer->setReadOnly(true);
    viewer->setObjectName("TerminalEdit");
    l->addWidget(viewer, 1);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    if (!task.url.isEmpty()) {
        QPushButton* openAdoBtn = new QPushButton("↗ Відкрити в Azure DevOps", &dlg);
        openAdoBtn->setObjectName("OutlineButton");
        connect(openAdoBtn, &QPushButton::clicked, &dlg, [u = task.url]() { WorkspaceManager::openExternalUrl(u); });
        btnRow->addWidget(openAdoBtn);
    }
    QPushButton* closeBtn = new QPushButton("Закрити", &dlg);
    closeBtn->setObjectName("PrimaryButton");
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(closeBtn);
    l->addLayout(btnRow);

    dlg.exec();
}

// ================= PAGE 1: TASK SETUP =================

QWidget* MainWindow::createSetupPage() {
    QWidget* page = new QWidget();
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

    QVBoxLayout* headerCol = new QVBoxLayout();
    headerCol->setSpacing(2);
    QLabel* h1 = new QLabel("Створення робочого оточення (task-setup)");
    h1->setStyleSheet("font-size: 22px; font-weight: 700; color: #ffffff;");
    QLabel* sub = new QLabel("Клонує репозиторій на найвільніший диск, ініціалізує субмодулі, створює гілку, генерує AI_PROMPT.md та запускає CMake.bat");
    sub->setStyleSheet("font-size: 13px; color: #94a3b8;");
    headerCol->addWidget(h1);
    headerCol->addWidget(sub);
    pageLayout->addLayout(headerCol);

    QHBoxLayout* contentRow = new QHBoxLayout();
    contentRow->setSpacing(18);

    // Left Card: Form
    QFrame* formCard = new QFrame();
    formCard->setObjectName("AgyCard");
    QVBoxLayout* formLayout = new QVBoxLayout(formCard);
    formLayout->setContentsMargins(20, 18, 20, 18);
    formLayout->setSpacing(14);

    QLabel* formTitle = new QLabel("Нове робоче оточення");
    formTitle->setStyleSheet("font-size: 16px; font-weight: 700; color: #ffffff;");
    formLayout->addWidget(formTitle);

    QLabel* mdLbl = new QLabel("Задача зі спринту (.md файл):");
    mdLbl->setStyleSheet("font-size: 12px; color: #94a3b8;");
    formLayout->addWidget(mdLbl);

    m_setupMdCombo = new QComboBox();
    formLayout->addWidget(m_setupMdCombo);

    QLabel* nameLbl = new QLabel("Коротка назва папки / гілки (наприклад: 12345-Oste-Rotate):");
    nameLbl->setStyleSheet("font-size: 12px; color: #94a3b8;");
    formLayout->addWidget(nameLbl);

    m_setupTaskInput = new QLineEdit();
    m_setupTaskInput->setPlaceholderText("12345-Short-Name");
    m_setupTaskInput->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace;");
    formLayout->addWidget(m_setupTaskInput);

    // Preview box
    QFrame* previewBox = new QFrame();
    previewBox->setObjectName("SubtleBox");
    QVBoxLayout* prevLayout = new QVBoxLayout(previewBox);
    prevLayout->setContentsMargins(12, 10, 12, 10);
    prevLayout->setSpacing(6);
    QLabel* prevHeader = new QLabel("Прев'ю параметрів:");
    prevHeader->setStyleSheet("font-size: 12px; font-weight: 600; color: #94a3b8;");
    m_setupFolderPreview = new QLabel();
    m_setupFolderPreview->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #34d399;");
    m_setupBranchPreview = new QLabel();
    m_setupBranchPreview->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #60a5fa;");
    prevLayout->addWidget(prevHeader);
    prevLayout->addWidget(m_setupFolderPreview);
    prevLayout->addWidget(m_setupBranchPreview);
    formLayout->addWidget(previewBox);

    connect(m_setupMdCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        QString mdPath = m_setupMdCombo->currentData().toString();
        if (!mdPath.isEmpty()) {
            for (const SprintTask& t : m_sprintTasks) {
                if (t.mdPath == mdPath) {
                    m_setupTaskInput->setText(WorkspaceManager::suggestShortTaskName(t.id, t.title));
                    break;
                }
            }
        }
        updateSetupPreview();
    });

    connect(m_setupTaskInput, &QLineEdit::textChanged, this, [this](const QString&) {
        updateSetupPreview();
    });

    QHBoxLayout* actionRow = new QHBoxLayout();
    m_setupStatusLabel = new QLabel("Готовий до запуску");
    m_setupStatusLabel->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #94a3b8;");
    actionRow->addWidget(m_setupStatusLabel);
    actionRow->addStretch();

    QPushButton* stopBtn = new QPushButton("⏹ Зупинити");
    stopBtn->setObjectName("DangerOutlineButton");
    stopBtn->setCursor(Qt::PointingHandCursor);
    connect(stopBtn, &QPushButton::clicked, m_setupRunner, &TaskSetupRunner::stop);
    actionRow->addWidget(stopBtn);

    m_setupRunBtn = new QPushButton("▶ Запустити task-setup");
    m_setupRunBtn->setObjectName("PrimaryButton");
    m_setupRunBtn->setCursor(Qt::PointingHandCursor);
    connect(m_setupRunBtn, &QPushButton::clicked, this, [this]() {
        QString taskName = m_setupTaskInput->text().trimmed();
        if (taskName.isEmpty()) {
            showStatusToast("Введіть коротку назву задачі!", true);
            return;
        }
        m_setupTerminal->clear();
        m_setupRunBtn->setEnabled(false);
        m_setupRunner->startSetup(taskName, m_setupMdCombo->currentData().toString());
    });
    actionRow->addWidget(m_setupRunBtn);

    formLayout->addLayout(actionRow);
    formLayout->addStretch();
    contentRow->addWidget(formCard, 5);

    // Right Card: Live Terminal
    QFrame* termCard = new QFrame();
    termCard->setObjectName("AgyCard");
    QVBoxLayout* termLayout = new QVBoxLayout(termCard);
    termLayout->setContentsMargins(20, 18, 20, 18);
    termLayout->setSpacing(10);

    QHBoxLayout* termHeader = new QHBoxLayout();
    QLabel* termTitle = new QLabel("💻 Живий вивід терміналу");
    termTitle->setStyleSheet("font-size: 16px; font-weight: 700; color: #ffffff;");
    termHeader->addWidget(termTitle);
    termHeader->addStretch();
    QPushButton* clearBtn = new QPushButton("Очистити");
    clearBtn->setObjectName("OutlineSmallButton");
    connect(clearBtn, &QPushButton::clicked, this, [this]() { m_setupTerminal->clear(); });
    termHeader->addWidget(clearBtn);
    termLayout->addLayout(termHeader);

    m_setupTerminal = new QPlainTextEdit();
    m_setupTerminal->setReadOnly(true);
    m_setupTerminal->setObjectName("TerminalEdit");
    termLayout->addWidget(m_setupTerminal, 1);

    contentRow->addWidget(termCard, 7);
    pageLayout->addLayout(contentRow, 1);

    refreshSprintTasksList();
    updateSetupPreview();
    return page;
}

void MainWindow::updateSetupPreview() {
    if (!m_setupFolderPreview || !m_setupBranchPreview) return;
    Settings cfg = Config::loadSettings();
    QList<DriveInfo> drives = WorkspaceManager::getDrivesInfo();
    DriveInfo best = drives.isEmpty()
        ? DriveInfo{"C:\\", QDir::toNativeSeparators(QDir::home().filePath("Git")), 0, 0, 0}
        : drives.first();

    QString tName = m_setupTaskInput ? m_setupTaskInput->text().trimmed() : QString();
    if (tName.isEmpty()) tName = "<task-name>";

    QString targetFolder = QDir::toNativeSeparators(QDir(best.root).filePath(tName));
    QString bPref = cfg.branchPrefix.isEmpty() ? cfg.repositoryId.toLower() : cfg.branchPrefix;
    QString branch = QString("%1/%2/%3").arg(bPref, cfg.userPrefix, tName.toLower());

    m_setupFolderPreview->setText(QString("📂 Папка:  %1 (%2 GB вільно)").arg(targetFolder).arg(best.freeGb, 0, 'f', 1));
    m_setupBranchPreview->setText(QString("🌿 Гілка:  %1").arg(branch));
}

// ================= PAGE 2: PR MONITOR =================

QWidget* MainWindow::createPrPage() {
    QWidget* page = new QWidget();
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

    QHBoxLayout* topRow = new QHBoxLayout();
    QVBoxLayout* titleCol = new QVBoxLayout();
    titleCol->setSpacing(2);
    QLabel* h1 = new QLabel("Моніторинг Pull Requests (watch-pr)");
    h1->setStyleSheet("font-size: 22px; font-weight: 700; color: #ffffff;");
    m_prSubtitleLabel = new QLabel();
    m_prSubtitleLabel->setStyleSheet("font-size: 13px; color: #94a3b8;");
    titleCol->addWidget(h1);
    titleCol->addWidget(m_prSubtitleLabel);
    topRow->addLayout(titleCol);

    topRow->addStretch();

    QCheckBox* monCheck = new QCheckBox("Фоновий моніторинг");
    monCheck->setChecked(m_prMonitoringActive);
    connect(monCheck, &QCheckBox::toggled, this, [this](bool v) { m_prMonitoringActive = v; });
    topRow->addWidget(monCheck);

    QCheckBox* notifyCheck = new QCheckBox("Toast-сповіщення");
    notifyCheck->setChecked(m_prNotifyEnabled);
    connect(notifyCheck, &QCheckBox::toggled, this, [this](bool v) { m_prNotifyEnabled = v; });
    topRow->addWidget(notifyCheck);

    QCheckBox* soundCheck = new QCheckBox("Звук");
    soundCheck->setChecked(m_prSoundEnabled);
    connect(soundCheck, &QCheckBox::toggled, this, [this](bool v) { m_prSoundEnabled = v; });
    topRow->addWidget(soundCheck);

    QPushButton* refreshBtn = new QPushButton("🔄 Оновити зараз");
    refreshBtn->setObjectName("PrimaryButton");
    refreshBtn->setCursor(Qt::PointingHandCursor);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        m_ado->fetchPullRequests(m_prIncludeDrafts);
    });
    topRow->addWidget(refreshBtn);

    pageLayout->addLayout(topRow);

    QHBoxLayout* bodyRow = new QHBoxLayout();
    bodyRow->setSpacing(18);

    // Left: Active PR cards in ScrollArea
    QScrollArea* prScroll = new QScrollArea();
    prScroll->setWidgetResizable(true);
    prScroll->setFrameShape(QFrame::NoFrame);
    QWidget* prScrollWidget = new QWidget();
    m_prCardsLayout = new QVBoxLayout(prScrollWidget);
    m_prCardsLayout->setContentsMargins(0, 0, 6, 0);
    m_prCardsLayout->setSpacing(12);
    prScroll->setWidget(prScrollWidget);
    bodyRow->addWidget(prScroll, 8);

    // Right: Live Events Feed
    QFrame* feedCard = new QFrame();
    feedCard->setObjectName("AgyCard");
    QVBoxLayout* feedCardLayout = new QVBoxLayout(feedCard);
    feedCardLayout->setContentsMargins(18, 16, 18, 16);
    feedCardLayout->setSpacing(10);

    QLabel* feedTitle = new QLabel("⚡ Журнал подій (Live Feed)");
    feedTitle->setStyleSheet("font-size: 15px; font-weight: 700; color: #ffffff;");
    feedCardLayout->addWidget(feedTitle);

    QScrollArea* feedScroll = new QScrollArea();
    feedScroll->setWidgetResizable(true);
    feedScroll->setFrameShape(QFrame::NoFrame);
    QWidget* feedScrollWidget = new QWidget();
    m_prEventsLayout = new QVBoxLayout(feedScrollWidget);
    m_prEventsLayout->setContentsMargins(0, 0, 4, 0);
    m_prEventsLayout->setSpacing(8);
    feedScroll->setWidget(feedScrollWidget);
    feedCardLayout->addWidget(feedScroll, 1);

    bodyRow->addWidget(feedCard, 4);
    pageLayout->addLayout(bodyRow, 1);

    refreshPrView();
    return page;
}

void MainWindow::refreshPrView() {
    Settings cfg = Config::loadSettings();
    if (m_prSubtitleLabel) {
        m_prSubtitleLabel->setText(QString("Останнє опитування: %1 • Автоматична перевірка кожні %2 сек")
                                       .arg(m_prLastCheck)
                                       .arg(cfg.prWatchInterval));
    }

    if (m_prCardsLayout) {
        clearLayout(m_prCardsLayout);

        if (!m_prError.isEmpty()) {
            QFrame* errCard = new QFrame();
            errCard->setStyleSheet("background-color: #2a1215; border: 1px solid #ef4444; border-radius: 10px;");
            QHBoxLayout* el = new QHBoxLayout(errCard);
            el->setContentsMargins(16, 12, 16, 12);
            QLabel* errLbl = new QLabel(QString("❌ Помилка з'єднання з Azure DevOps: %1").arg(m_prError));
            errLbl->setStyleSheet("color: #fca5a5; font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px;");
            el->addWidget(errLbl);
            m_prCardsLayout->addWidget(errCard);
        }

        if (m_prSnapshots.isEmpty()) {
            QFrame* empty = new QFrame();
            empty->setObjectName("AgyCard");
            QVBoxLayout* el = new QVBoxLayout(empty);
            el->setContentsMargins(24, 36, 24, 36);
            QLabel* lbl = new QLabel("✅ Немає активних Pull Requests (або триває перше завантаження).");
            lbl->setAlignment(Qt::AlignCenter);
            lbl->setStyleSheet("color: #cbd5e1; font-size: 14px;");
            el->addWidget(lbl);
            m_prCardsLayout->addWidget(empty);
        } else {
            for (auto it = m_prSnapshots.cbegin(); it != m_prSnapshots.cend(); ++it) {
                const PrSnapshot& snap = it.value();
                QFrame* card = new QFrame();
                card->setObjectName("AgyCard");
                QVBoxLayout* cl = new QVBoxLayout(card);
                cl->setContentsMargins(18, 16, 18, 16);
                cl->setSpacing(12);

                // Header
                QHBoxLayout* hdr = new QHBoxLayout();
                QVBoxLayout* titleBox = new QVBoxLayout();
                titleBox->setSpacing(4);
                QHBoxLayout* prTitleRow = new QHBoxLayout();
                QLabel* idBadge = new QLabel(QString("PR #%1").arg(snap.id));
                idBadge->setStyleSheet("background-color: #1d4ed8; color: #ffffff; border-radius: 4px; padding: 2px 8px; font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 11px; font-weight: 700;");
                QLabel* titleLbl = new QLabel(snap.title);
                titleLbl->setStyleSheet("font-size: 15px; font-weight: 700; color: #ffffff;");
                prTitleRow->addWidget(idBadge);
                prTitleRow->addWidget(titleLbl, 1);
                titleBox->addLayout(prTitleRow);

                QLabel* branchLbl = new QLabel(QString("🌿 %1").arg(snap.branch));
                branchLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #22d3ee;");
                titleBox->addWidget(branchLbl);
                hdr->addLayout(titleBox, 1);

                QPushButton* openBtn = new QPushButton("↗ Відкрити в ADO");
                openBtn->setObjectName("OutlineSmallButton");
                openBtn->setCursor(Qt::PointingHandCursor);
                connect(openBtn, &QPushButton::clicked, this, [u = snap.url]() { WorkspaceManager::openExternalUrl(u); });
                hdr->addWidget(openBtn);
                cl->addLayout(hdr);

                // Gated Builds box
                QFrame* buildsBox = new QFrame();
                buildsBox->setObjectName("SubtleBox");
                QVBoxLayout* bl = new QVBoxLayout(buildsBox);
                bl->setContentsMargins(12, 10, 12, 10);
                bl->setSpacing(4);
                QLabel* bHdr = new QLabel("Gated Builds");
                bHdr->setStyleSheet("font-size: 12px; font-weight: 600; color: #94a3b8;");
                bl->addWidget(bHdr);

                if (snap.builds.isEmpty()) {
                    QLabel* noneLbl = new QLabel("Немає налаштованих Gated білдів");
                    noneLbl->setStyleSheet("font-size: 12px; color: #64748b;");
                    bl->addWidget(noneLbl);
                } else {
                    for (auto bIt = snap.builds.cbegin(); bIt != snap.builds.cend(); ++bIt) {
                        QHBoxLayout* bRow = new QHBoxLayout();
                        QLabel* bName = new QLabel(bIt.key());
                        bName->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #e2e8f0;");
                        QString st = bIt.value().toLower();
                        QString col = (st == "approved" || st == "succeeded") ? "#34d399"
                                    : (st == "rejected" || st == "failed" || st == "broken") ? "#f87171"
                                    : "#fbbf24";
                        QLabel* bSt = new QLabel(bIt.value().toUpper());
                        bSt->setStyleSheet(QString("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; font-weight: 700; color: %1;").arg(col));
                        bRow->addWidget(bName);
                        bRow->addStretch();
                        bRow->addWidget(bSt);
                        bl->addLayout(bRow);
                    }
                }
                cl->addWidget(buildsBox);

                // Bottom row: Active Comments + Reviewers
                QHBoxLayout* bottomRow = new QHBoxLayout();
                bottomRow->setSpacing(12);

                QFrame* commBox = new QFrame();
                commBox->setObjectName("SubtleBox");
                QVBoxLayout* cml = new QVBoxLayout(commBox);
                cml->setContentsMargins(12, 10, 12, 10);
                cml->setSpacing(4);
                QLabel* cHdr = new QLabel(QString("Активні коментарі (%1)").arg(snap.activeThreads.size()));
                cHdr->setStyleSheet("font-size: 12px; font-weight: 600; color: #94a3b8;");
                cml->addWidget(cHdr);
                if (snap.activeThreads.isEmpty()) {
                    QLabel* okLbl = new QLabel("Усі треди вирішені ✅");
                    okLbl->setStyleSheet("font-size: 12px; color: #34d399;");
                    cml->addWidget(okLbl);
                } else {
                    int shown = 0;
                    for (const PrActiveThread& th : snap.activeThreads) {
                        if (++shown > 5) break;
                        QLabel* cLbl = new QLabel(QString("💬 %1: %2").arg(th.lastAuthor, th.lastSnippet));
                        cLbl->setWordWrap(true);
                        cLbl->setStyleSheet("font-size: 12px; color: #fcd34d;");
                        cml->addWidget(cLbl);
                    }
                }
                cml->addStretch();
                bottomRow->addWidget(commBox, 1);

                QFrame* revBox = new QFrame();
                revBox->setObjectName("SubtleBox");
                QVBoxLayout* rvl = new QVBoxLayout(revBox);
                rvl->setContentsMargins(12, 10, 12, 10);
                rvl->setSpacing(4);
                QLabel* rHdr = new QLabel("Голоси рев'юверів");
                rHdr->setStyleSheet("font-size: 12px; font-weight: 600; color: #94a3b8;");
                rvl->addWidget(rHdr);

                bool anyVotes = false;
                for (auto rIt = snap.reviewers.cbegin(); rIt != snap.reviewers.cend(); ++rIt) {
                    if (rIt.value() == 0) continue;
                    anyVotes = true;
                    QHBoxLayout* rRow = new QHBoxLayout();
                    QLabel* rName = new QLabel(rIt.key());
                    rName->setStyleSheet("font-size: 12px; color: #e2e8f0;");
                    int v = rIt.value();
                    QString vTxt = (v == 10) ? "Approved (+10)"
                                 : (v == 5)  ? "Suggestions (+5)"
                                 : (v == -5) ? "Waiting (-5)"
                                             : QString("Rejected (%1)").arg(v);
                    QString vCol = (v > 0) ? "#34d399" : "#f87171";
                    QLabel* rVote = new QLabel(vTxt);
                    rVote->setStyleSheet(QString("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: %1;").arg(vCol));
                    rRow->addWidget(rName);
                    rRow->addStretch();
                    rRow->addWidget(rVote);
                    rvl->addLayout(rRow);
                }
                if (!anyVotes) {
                    QLabel* noVotes = new QLabel("Ще немає голосів");
                    noVotes->setStyleSheet("font-size: 12px; color: #64748b;");
                    rvl->addWidget(noVotes);
                }
                rvl->addStretch();
                bottomRow->addWidget(revBox, 1);

                cl->addLayout(bottomRow);
                m_prCardsLayout->addWidget(card);
            }
        }
        m_prCardsLayout->addStretch();
    }

    if (m_prEventsLayout) {
        clearLayout(m_prEventsLayout);
        if (m_prEvents.isEmpty()) {
            QLabel* emptyLbl = new QLabel("Поки що змін у статусах PR не зафіксовано.");
            emptyLbl->setStyleSheet("font-size: 12px; color: #64748b;");
            m_prEventsLayout->addWidget(emptyLbl);
        } else {
            for (const PrEvent& ev : m_prEvents) {
                QFrame* evBox = new QFrame();
                evBox->setObjectName("SubtleBox");
                QHBoxLayout* el = new QHBoxLayout(evBox);
                el->setContentsMargins(10, 8, 10, 8);
                el->setSpacing(8);
                QLabel* tLbl = new QLabel(ev.time);
                tLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 11px; color: #64748b;");
                QLabel* txtLbl = new QLabel(ev.text);
                txtLbl->setWordWrap(true);
                txtLbl->setStyleSheet("font-size: 12px; color: #e2e8f0;");
                el->addWidget(tLbl);
                el->addWidget(txtLbl, 1);
                m_prEventsLayout->addWidget(evBox);
            }
        }
        m_prEventsLayout->addStretch();
    }
}

// ================= PAGE 3: DISK USAGE (KEEP SPACE) =================

QWidget* MainWindow::createSpacePage() {
    QWidget* page = new QWidget();
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(14);

    QHBoxLayout* topRow = new QHBoxLayout();
    QVBoxLayout* titleCol = new QVBoxLayout();
    titleCol->setSpacing(2);
    QLabel* h1 = new QLabel("Disk Usage — Очищення робочих папок");
    h1->setStyleSheet("font-size: 22px; font-weight: 700; color: #ffffff;");
    QLabel* sub = new QLabel("Список робочих директорій у git_roots зі статусами задач зі спринту для швидкого видалення закритих тасок");
    sub->setStyleSheet("font-size: 13px; color: #94a3b8;");
    titleCol->addWidget(h1);
    titleCol->addWidget(sub);
    topRow->addLayout(titleCol);
    topRow->addStretch();

    QPushButton* scanBtn = new QPushButton("🔄 Оновити список");
    scanBtn->setObjectName("OutlineButton");
    scanBtn->setCursor(Qt::PointingHandCursor);
    connect(scanBtn, &QPushButton::clicked, this, &MainWindow::refreshSpaceView);
    topRow->addWidget(scanBtn);

    QPushButton* selClosedBtn = new QPushButton("☑️ Вибрати всі закриті");
    selClosedBtn->setObjectName("OutlineButton");
    selClosedBtn->setCursor(Qt::PointingHandCursor);
    connect(selClosedBtn, &QPushButton::clicked, this, [this]() {
        for (const WorkspaceFolder& w : m_workspaces) {
            if (w.isClosed) m_selectedWorkspaces.insert(w.path);
        }
        refreshSpaceView();
    });
    topRow->addWidget(selClosedBtn);

    QPushButton* delBtn = new QPushButton("🗑️ Видалити вибрані");
    delBtn->setObjectName("DangerButton");
    delBtn->setCursor(Qt::PointingHandCursor);
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        if (m_selectedWorkspaces.isEmpty() || m_isDeletingWorkspaces) {
            showStatusToast("Спочатку виберіть папки для видалення.", true);
            return;
        }
        QStringList toDelete = m_selectedWorkspaces.values();
        toDelete.sort();

        QMessageBox msgBox(this);
        msgBox.setWindowTitle("Підтвердження видалення");
        msgBox.setText(QString("Ви дійсно хочете назавжди видалити вибрані папки (%1 шт.)?").arg(toDelete.size()));
        msgBox.setDetailedText(toDelete.join("\n"));
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        if (msgBox.exec() != QMessageBox::Yes) return;

        m_isDeletingWorkspaces = true;
        showStatusToast(QString("⏳ Видалення %1 папок...").arg(toDelete.size()));

        (void)QtConcurrent::run([this, toDelete]() {
            int deletedCount = 0;
            for (const QString& p : toDelete) {
                if (WorkspaceManager::deleteDirectoryForce(p)) {
                    deletedCount++;
                }
            }
            QMetaObject::invokeMethod(this, [this, deletedCount]() {
                m_isDeletingWorkspaces = false;
                showStatusToast(QString("✅ Видалено %1 папок!").arg(deletedCount));
                refreshDrivesWidget();
                refreshSpaceView();
                refreshSprintTasksList();
            });
        });
    });
    topRow->addWidget(delBtn);
    pageLayout->addLayout(topRow);

    // 3 Stat Cards
    QHBoxLayout* statsRow = new QHBoxLayout();
    statsRow->setSpacing(14);
    auto makeStatCard = [&](const QString& label, QLabel*& valLabel, const QString& color) {
        QFrame* c = new QFrame();
        c->setObjectName("AgyCard");
        QVBoxLayout* l = new QVBoxLayout(c);
        l->setContentsMargins(16, 12, 16, 12);
        QLabel* t = new QLabel(label);
        t->setStyleSheet("font-size: 12px; color: #94a3b8;");
        valLabel = new QLabel("0 папок");
        valLabel->setStyleSheet(QString("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 20px; font-weight: 700; color: %1;").arg(color));
        l->addWidget(t);
        l->addWidget(valLabel);
        statsRow->addWidget(c, 1);
    };
    makeStatCard("Усього робочих папок", m_spaceTotalLabel, "#ffffff");
    makeStatCard("Закриті таски (Resolved / Closed)", m_spaceClosedLabel, "#34d399");
    makeStatCard("Вибрано до видалення", m_spaceSelectedLabel, "#f87171");
    pageLayout->addLayout(statsRow);

    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget* scrollContent = new QWidget();
    m_spaceListLayout = new QVBoxLayout(scrollContent);
    m_spaceListLayout->setContentsMargins(0, 0, 6, 0);
    m_spaceListLayout->setSpacing(8);
    scroll->setWidget(scrollContent);
    pageLayout->addWidget(scroll, 1);

    // Initial scan: preselect closed workspaces
    m_workspaces = WorkspaceManager::scanWorkspaces();
    for (const WorkspaceFolder& w : m_workspaces) {
        if (w.isClosed) m_selectedWorkspaces.insert(w.path);
    }
    refreshSpaceView();
    return page;
}

void MainWindow::refreshSpaceView() {
    m_workspaces = WorkspaceManager::scanWorkspaces();

    // Keep only still-existing selected paths
    QSet<QString> validPaths;
    int closedCount = 0;
    for (const WorkspaceFolder& w : m_workspaces) {
        validPaths.insert(w.path);
        if (w.isClosed) closedCount++;
    }
    m_selectedWorkspaces.intersect(validPaths);

    if (m_spaceTotalLabel) m_spaceTotalLabel->setText(QString("%1 папок").arg(m_workspaces.size()));
    if (m_spaceClosedLabel) m_spaceClosedLabel->setText(QString("%1 папок").arg(closedCount));
    if (m_spaceSelectedLabel) m_spaceSelectedLabel->setText(QString("%1 папок").arg(m_selectedWorkspaces.size()));

    if (!m_spaceListLayout) return;
    clearLayout(m_spaceListLayout);

    if (m_workspaces.isEmpty()) {
        QFrame* empty = new QFrame();
        empty->setObjectName("AgyCard");
        QVBoxLayout* el = new QVBoxLayout(empty);
        el->setContentsMargins(24, 32, 24, 32);
        QLabel* lbl = new QLabel("📁 Робочих папок у git_roots не знайдено.");
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("color: #94a3b8; font-size: 14px;");
        el->addWidget(lbl);
        m_spaceListLayout->addWidget(empty);
        m_spaceListLayout->addStretch();
        return;
    }

    for (const WorkspaceFolder& w : m_workspaces) {
        bool isChecked = m_selectedWorkspaces.contains(w.path);
        QFrame* card = new QFrame();
        card->setObjectName(isChecked ? "AgyCardDanger" : "AgyCard");
        QHBoxLayout* row = new QHBoxLayout(card);
        row->setContentsMargins(14, 10, 14, 10);
        row->setSpacing(12);

        QCheckBox* cb = new QCheckBox();
        cb->setChecked(isChecked);
        connect(cb, &QCheckBox::toggled, this, [this, p = w.path](bool checked) {
            if (checked) m_selectedWorkspaces.insert(p);
            else m_selectedWorkspaces.remove(p);
            refreshSpaceView();
        });
        row->addWidget(cb);

        QVBoxLayout* infoCol = new QVBoxLayout();
        infoCol->setSpacing(3);
        QHBoxLayout* nameRow = new QHBoxLayout();
        nameRow->setSpacing(8);
        QLabel* nameLbl = new QLabel(w.name);
        nameLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 13px; font-weight: 700; color: #ffffff;");
        nameRow->addWidget(nameLbl);

        QLabel* stBadge = new QLabel(w.state);
        stBadge->setStyleSheet(stateBadgeStyle(w.state));
        nameRow->addWidget(stBadge);

        if (w.isClosed) {
            QLabel* recBadge = new QLabel("Рекомендовано видалити");
            recBadge->setStyleSheet("background-color: #064e3b; color: #6ee7b7; border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: 600;");
            nameRow->addWidget(recBadge);
        }
        nameRow->addStretch();
        infoCol->addLayout(nameRow);

        QString subText = !w.taskId.isEmpty() ? QString("#%1 %2").arg(w.taskId, w.taskTitle) : w.path;
        QLabel* subLbl = new QLabel(subText);
        subLbl->setStyleSheet("font-size: 12px; color: #94a3b8;");
        infoCol->addWidget(subLbl);
        row->addLayout(infoCol, 1);

        QLabel* pathLbl = new QLabel(w.path);
        pathLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 11px; color: #64748b;");
        row->addWidget(pathLbl);

        QPushButton* expBtn = new QPushButton("📂");
        expBtn->setObjectName("OutlineSmallButton");
        expBtn->setToolTip("Відкрити в Explorer");
        connect(expBtn, &QPushButton::clicked, this, [p = w.path]() { WorkspaceManager::openInExplorer(p); });
        row->addWidget(expBtn);

        QPushButton* codeBtn = new QPushButton("💻");
        codeBtn->setObjectName("OutlineSmallButton");
        codeBtn->setToolTip("Відкрити у Antigravity / VS Code");
        connect(codeBtn, &QPushButton::clicked, this, [p = w.path]() { WorkspaceManager::openInCodeEditor(p); });
        row->addWidget(codeBtn);

        QPushButton* prBtn = new QPushButton("🔀 PR");
        prBtn->setObjectName("PrimarySmallButton");
        prBtn->setToolTip("Створити Pull Request з цієї робочої папки");
        connect(prBtn, &QPushButton::clicked, this, [this, p = w.path]() { openCreatePrDialog(p); });
        row->addWidget(prBtn);

        m_spaceListLayout->addWidget(card);
    }
    m_spaceListLayout->addStretch();
}

// ================= PAGE 4: LINKS & SETTINGS =================

QWidget* MainWindow::createLinksSettingsPage() {
    QWidget* page = new QWidget();
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(24, 20, 24, 20);

    QTabWidget* tabs = new QTabWidget();

    // ---- Tab 1: Quick Links ----
    QWidget* linksTab = new QWidget();
    QVBoxLayout* linksTabLayout = new QVBoxLayout(linksTab);
    linksTabLayout->setContentsMargins(0, 16, 0, 0);
    linksTabLayout->setSpacing(14);

    QHBoxLayout* linksHeader = new QHBoxLayout();
    QLabel* lh = new QLabel("Швидкі ресурси та закладки (data/links.json)");
    lh->setStyleSheet("font-size: 18px; font-weight: 700; color: #ffffff;");
    linksHeader->addWidget(lh);
    linksHeader->addStretch();

    QPushButton* addLinkBtn = new QPushButton("➕ Додати лінк");
    addLinkBtn->setObjectName("PrimaryButton");
    addLinkBtn->setCursor(Qt::PointingHandCursor);
    connect(addLinkBtn, &QPushButton::clicked, this, [this]() {
        QDialog dlg(this);
        dlg.setWindowTitle("Новий ресурс");
        dlg.setFixedWidth(420);
        QVBoxLayout* l = new QVBoxLayout(&dlg);
        QFormLayout* form = new QFormLayout();
        QLineEdit* tIn = new QLineEdit(&dlg);
        QLineEdit* cIn = new QLineEdit("Azure DevOps", &dlg);
        QLineEdit* uIn = new QLineEdit(&dlg);
        uIn->setPlaceholderText("https://...");
        QLineEdit* dIn = new QLineEdit(&dlg);
        form->addRow("Назва:", tIn);
        form->addRow("Категорія:", cIn);
        form->addRow("URL:", uIn);
        form->addRow("Короткий опис:", dIn);
        l->addLayout(form);

        QHBoxLayout* btns = new QHBoxLayout();
        btns->addStretch();
        QPushButton* cancelB = new QPushButton("Скасувати", &dlg);
        QPushButton* saveB = new QPushButton("Зберегти", &dlg);
        saveB->setObjectName("PrimaryButton");
        connect(cancelB, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(saveB, &QPushButton::clicked, &dlg, &QDialog::accept);
        btns->addWidget(cancelB);
        btns->addWidget(saveB);
        l->addLayout(btns);

        if (dlg.exec() == QDialog::Accepted) {
            if (tIn->text().trimmed().isEmpty() || uIn->text().trimmed().isEmpty()) {
                showStatusToast("Заповніть назву та URL!", true);
                return;
            }
            QList<QuickLink> links = Config::loadLinks();
            QuickLink nl;
            nl.title = tIn->text().trimmed();
            nl.category = cIn->text().trimmed().isEmpty() ? "Інше" : cIn->text().trimmed();
            nl.url = uIn->text().trimmed();
            nl.description = dIn->text().trimmed();
            links.append(nl);
            Config::saveLinks(links);
            refreshLinksView();
        }
    });
    linksHeader->addWidget(addLinkBtn);
    linksTabLayout->addLayout(linksHeader);

    QScrollArea* linksScroll = new QScrollArea();
    linksScroll->setWidgetResizable(true);
    linksScroll->setFrameShape(QFrame::NoFrame);
    QWidget* linksScrollWidget = new QWidget();
    m_linksGridLayout = new QVBoxLayout(linksScrollWidget);
    m_linksGridLayout->setContentsMargins(0, 0, 6, 0);
    linksScroll->setWidget(linksScrollWidget);
    linksTabLayout->addWidget(linksScroll, 1);

    tabs->addTab(linksTab, "🔖 Швидкі лінки (Bookmarks)");

    // ---- Tab 2: Settings (data/settings.json) ----
    QWidget* cfgTab = new QWidget();
    QVBoxLayout* cfgOuter = new QVBoxLayout(cfgTab);
    cfgOuter->setContentsMargins(0, 16, 0, 0);

    QFrame* cfgCard = new QFrame();
    cfgCard->setObjectName("AgyCard");
    cfgCard->setMaximumWidth(820);
    QVBoxLayout* cfgLayout = new QVBoxLayout(cfgCard);
    cfgLayout->setContentsMargins(22, 20, 22, 20);
    cfgLayout->setSpacing(12);

    QLabel* cfgTitle = new QLabel("Локальна конфігурація (data/settings.json)");
    cfgTitle->setStyleSheet("font-size: 17px; font-weight: 700; color: #ffffff;");
    QLabel* cfgNote = new QLabel("Цей файл додано в .gitignore, тому твої корпоративні назви та шляхи ніколи не потраплять у публічний GitHub.");
    cfgNote->setStyleSheet("font-size: 12px; color: #34d399;");
    cfgLayout->addWidget(cfgTitle);
    cfgLayout->addWidget(cfgNote);

    Settings s = Config::loadSettings();
    QGridLayout* grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(10);

    m_cfgOrg = new QLineEdit(s.organization);
    m_cfgProj = new QLineEdit(s.project);
    m_cfgRepo = new QLineEdit(s.repositoryId);
    grid->addWidget(new QLabel("Organization (ADO):"), 0, 0);
    grid->addWidget(m_cfgOrg, 1, 0);
    grid->addWidget(new QLabel("Project (ADO):"), 0, 1);
    grid->addWidget(m_cfgProj, 1, 1);
    grid->addWidget(new QLabel("Repository ID:"), 0, 2);
    grid->addWidget(m_cfgRepo, 1, 2);

    m_cfgQuery = new QLineEdit(s.queryId);
    m_cfgTarget = new QLineEdit(s.targetBranch);
    m_cfgPrInterval = new QSpinBox();
    m_cfgPrInterval->setRange(10, 600);
    m_cfgPrInterval->setValue(s.prWatchInterval);
    grid->addWidget(new QLabel("Sprint WIQL Query ID:"), 2, 0, 1, 2);
    grid->addWidget(m_cfgQuery, 3, 0, 1, 2);
    grid->addWidget(new QLabel("Target Branch (PR):"), 2, 2);
    grid->addWidget(m_cfgTarget, 3, 2);

    m_cfgBranchPref = new QLineEdit(s.branchPrefix);
    m_cfgUserPref = new QLineEdit(s.userPrefix);
    grid->addWidget(new QLabel("Branch Prefix:"), 4, 0);
    grid->addWidget(m_cfgBranchPref, 5, 0);
    grid->addWidget(new QLabel("User Prefix:"), 4, 1);
    grid->addWidget(m_cfgUserPref, 5, 1);
    grid->addWidget(new QLabel("PR Watch Interval (сек):"), 4, 2);
    grid->addWidget(m_cfgPrInterval, 5, 2);

    m_cfgGitRoots = new QLineEdit(s.gitRoots.join(", "));
    m_cfgClang = new QLineEdit(s.clangFormatPath);
    grid->addWidget(new QLabel("Git Roots (через кому):"), 6, 0, 1, 3);
    grid->addWidget(m_cfgGitRoots, 7, 0, 1, 3);
    grid->addWidget(new QLabel("Шлях до clang-format.exe:"), 8, 0, 1, 3);
    grid->addWidget(m_cfgClang, 9, 0, 1, 3);

    cfgLayout->addLayout(grid);

    QHBoxLayout* saveRow = new QHBoxLayout();
    saveRow->addStretch();
    QPushButton* saveCfgBtn = new QPushButton("💾 Зберегти налаштування");
    saveCfgBtn->setObjectName("PrimaryButton");
    saveCfgBtn->setCursor(Qt::PointingHandCursor);
    connect(saveCfgBtn, &QPushButton::clicked, this, [this]() {
        Settings ns = Config::loadSettings();
        ns.organization = m_cfgOrg->text().trimmed();
        ns.project = m_cfgProj->text().trimmed();
        ns.repositoryId = m_cfgRepo->text().trimmed();
        ns.queryId = m_cfgQuery->text().trimmed();
        ns.targetBranch = m_cfgTarget->text().trimmed();
        ns.branchPrefix = m_cfgBranchPref->text().trimmed();
        ns.userPrefix = m_cfgUserPref->text().trimmed();
        ns.prWatchInterval = m_cfgPrInterval->value();
        ns.clangFormatPath = m_cfgClang->text().trimmed();
        ns.prWatchNotify = m_prNotifyEnabled;
        ns.prWatchSound = m_prSoundEnabled;
        ns.prWatchIncludeDrafts = m_prIncludeDrafts;

        QStringList roots;
        for (const QString& part : m_cfgGitRoots->text().split(',')) {
            if (!part.trimmed().isEmpty()) roots.append(part.trimmed());
        }
        ns.gitRoots = roots;

        if (Config::saveSettings(ns)) {
            m_prTimer->setInterval(qMax(15, ns.prWatchInterval) * 1000);
            refreshDrivesWidget();
            updateSetupPreview();
            showStatusToast("Налаштування збережено у data/settings.json!");
        } else {
            showStatusToast("Не вдалося зберегти data/settings.json!", true);
        }
    });
    saveRow->addWidget(saveCfgBtn);
    cfgLayout->addLayout(saveRow);

    cfgOuter->addWidget(cfgCard);
    cfgOuter->addStretch();
    tabs->addTab(cfgTab, "⚙️ Налаштування (settings.json)");

    pageLayout->addWidget(tabs);
    refreshLinksView();
    return page;
}

void MainWindow::refreshLinksView() {
    if (!m_linksGridLayout) return;
    clearLayout(m_linksGridLayout);

    QList<QuickLink> links = Config::loadLinks();
    QGridLayout* grid = new QGridLayout();
    grid->setSpacing(14);

    for (int i = 0; i < links.size(); ++i) {
        const QuickLink& item = links[i];
        QFrame* card = new QFrame();
        card->setObjectName("AgyCardHover");
        card->setFixedWidth(320);
        QVBoxLayout* cl = new QVBoxLayout(card);
        cl->setContentsMargins(16, 14, 16, 14);
        cl->setSpacing(8);

        QHBoxLayout* top = new QHBoxLayout();
        QLabel* catBadge = new QLabel(item.category);
        catBadge->setStyleSheet("background-color: #1e3a8a; color: #93c5fd; border-radius: 4px; padding: 2px 8px; font-size: 11px; font-weight: 600;");
        top->addWidget(catBadge);
        top->addStretch();

        QPushButton* delB = new QPushButton("✕");
        delB->setFixedSize(24, 24);
        delB->setObjectName("OutlineSmallButton");
        connect(delB, &QPushButton::clicked, this, [this, i]() {
            QList<QuickLink> cur = Config::loadLinks();
            if (i >= 0 && i < cur.size()) {
                cur.removeAt(i);
                Config::saveLinks(cur);
                refreshLinksView();
            }
        });
        top->addWidget(delB);
        cl->addLayout(top);

        QLabel* tLbl = new QLabel(item.title);
        tLbl->setStyleSheet("font-size: 15px; font-weight: 700; color: #ffffff;");
        cl->addWidget(tLbl);

        QLabel* dLbl = new QLabel(item.description);
        dLbl->setWordWrap(true);
        dLbl->setStyleSheet("font-size: 12px; color: #94a3b8;");
        cl->addWidget(dLbl, 1);

        QPushButton* openB = new QPushButton("↗ Відкрити");
        openB->setObjectName("OutlineButton");
        openB->setCursor(Qt::PointingHandCursor);
        connect(openB, &QPushButton::clicked, this, [u = item.url]() { WorkspaceManager::openExternalUrl(u); });
        cl->addWidget(openB);

        grid->addWidget(card, i / 3, i % 3);
    }

    QHBoxLayout* wrap = new QHBoxLayout();
    wrap->addLayout(grid);
    wrap->addStretch();
    m_linksGridLayout->addLayout(wrap);
    m_linksGridLayout->addStretch();
}

// ================= CREATE PR DIALOG (create-pr.py replacement) =================

void MainWindow::openCreatePrDialog(const QString& workspacePath) {
    QString branch, defaultTitle, err;
    int taskId = 0;
    if (!AdoClient::inspectWorkspaceForPr(workspacePath, branch, defaultTitle, taskId, err)) {
        QMessageBox::warning(this, "Створення Pull Request", err);
        return;
    }

    Settings cfg = Config::loadSettings();
    QDialog dlg(this);
    dlg.setWindowTitle("Створення Pull Request (create-pr)");
    dlg.setFixedWidth(560);

    QVBoxLayout* l = new QVBoxLayout(&dlg);
    l->setContentsMargins(20, 18, 20, 18);
    l->setSpacing(12);

    QLabel* wsLbl = new QLabel(QString("📂 Робоча папка: %1").arg(workspacePath));
    wsLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #94a3b8;");
    l->addWidget(wsLbl);

    QLabel* brLbl = new QLabel(QString("🌿 Гілка: %1  →  %2").arg(branch, cfg.targetBranch));
    brLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #34d399; font-weight: 600;");
    l->addWidget(brLbl);

    if (taskId > 0) {
        QLabel* wiLbl = new QLabel(QString("🔗 Прив'язка до Work Item: #%1").arg(taskId));
        wiLbl->setStyleSheet("font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 12px; color: #60a5fa;");
        l->addWidget(wiLbl);
    }

    l->addWidget(new QLabel("Заголовок Pull Request:"));
    QLineEdit* titleEdit = new QLineEdit(defaultTitle, &dlg);
    l->addWidget(titleEdit);

    QPlainTextEdit* logView = new QPlainTextEdit(&dlg);
    logView->setReadOnly(true);
    logView->setObjectName("TerminalEdit");
    logView->setFixedHeight(120);
    l->addWidget(logView);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* closeBtn = new QPushButton("Закрити", &dlg);
    QPushButton* createBtn = new QPushButton("🚀 Git Push & Створити PR", &dlg);
    createBtn->setObjectName("PrimaryButton");
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(createBtn);
    l->addLayout(btnRow);

    QMetaObject::Connection c1 = connect(m_ado, &AdoClient::createPrLog, &dlg, [logView](const QString& line) {
        logView->appendPlainText(line);
    });
    QMetaObject::Connection c2 = connect(m_ado, &AdoClient::createPrFinished, &dlg, [this, logView, createBtn](bool ok, const QString& prUrl, const QString& msg) {
        createBtn->setEnabled(true);
        logView->appendPlainText(ok ? ("\n✅ " + msg) : ("\n❌ " + msg));
        if (ok && !prUrl.isEmpty()) {
            logView->appendPlainText("URL: " + prUrl);
            WorkspaceManager::openExternalUrl(prUrl);
            m_ado->fetchPullRequests(m_prIncludeDrafts);
        }
        showStatusToast(msg, !ok);
    });

    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(createBtn, &QPushButton::clicked, &dlg, [this, workspacePath, titleEdit, createBtn, logView]() {
        createBtn->setEnabled(false);
        logView->clear();
        m_ado->createPullRequest(workspacePath, titleEdit->text());
    });

    dlg.exec();
    disconnect(c1);
    disconnect(c2);
}
