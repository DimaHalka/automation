#include "MainWindow.h"

#include <QApplication>
#include <QFont>
#include <QStyleFactory>

static const char* ANTIGRAVITY_QSS = R"(
QMainWindow, QWidget {
    background-color: #0b0d12;
    color: #e2e8f0;
    font-family: "Segoe UI", "Inter", sans-serif;
    font-size: 13px;
}

QDialog {
    background-color: #0f1219;
}

#HeaderBar {
    background-color: #0f1219;
    border-bottom: 1px solid #1e2433;
}

#Sidebar {
    background-color: #0d1017;
    border-right: 1px solid #1e2433;
}

#NavButton {
    background-color: transparent;
    color: #94a3b8;
    border: none;
    border-left: 3px solid transparent;
    border-radius: 6px;
    padding: 10px 12px;
    text-align: left;
    font-size: 13px;
    font-weight: 600;
}

#NavButton:hover {
    background-color: #161b26;
    color: #f8fafc;
}

#NavButton[navActive="true"] {
    background-color: rgba(59, 130, 246, 0.14);
    color: #60a5fa;
    border-left: 3px solid #3b82f6;
}

#DriveCard {
    background-color: #121620;
    border: 1px solid #1e2536;
    border-radius: 8px;
}

#AgyCard {
    background-color: #121620;
    border: 1px solid #1e2536;
    border-radius: 10px;
}

#AgyCardHover {
    background-color: #121620;
    border: 1px solid #1e2536;
    border-radius: 10px;
}

#AgyCardHover:hover {
    border: 1px solid #3b82f6;
}

#AgyCardDanger {
    background-color: #1c1218;
    border: 1px solid #ef4444;
    border-radius: 10px;
}

#SubtleBox {
    background-color: #080a0f;
    border: 1px solid #1e2433;
    border-radius: 8px;
}

#SubtleBox QLabel {
    background: transparent;
}

#AgyCard QLabel, #AgyCardHover QLabel, #AgyCardDanger QLabel, #DriveCard QLabel {
    background: transparent;
}

#TerminalEdit {
    background-color: #080a0f;
    border: 1px solid #1e2433;
    border-radius: 8px;
    color: #cbd5e1;
    font-family: "Cascadia Mono", "Consolas", monospace;
    font-size: 12px;
    padding: 8px;
}

QLineEdit, QComboBox, QSpinBox {
    background-color: #090b10;
    border: 1px solid #262f45;
    border-radius: 6px;
    padding: 6px 10px;
    color: #f1f5f9;
    selection-background-color: #2563eb;
}

QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
    border: 1px solid #3b82f6;
}

QComboBox::drop-down {
    border: none;
    width: 24px;
}

QComboBox QAbstractItemView {
    background-color: #121620;
    border: 1px solid #262f45;
    color: #f1f5f9;
    selection-background-color: #1d4ed8;
}

QPushButton {
    background-color: #1e293b;
    color: #e2e8f0;
    border: 1px solid #334155;
    border-radius: 6px;
    padding: 6px 14px;
    font-weight: 600;
}

QPushButton:hover {
    background-color: #334155;
}

QPushButton:disabled {
    background-color: #141824;
    color: #475569;
    border-color: #1e2433;
}

#PrimaryButton {
    background-color: #2563eb;
    color: #ffffff;
    border: 1px solid #3b82f6;
    border-radius: 6px;
    padding: 7px 16px;
    font-weight: 600;
}

#PrimaryButton:hover {
    background-color: #1d4ed8;
}

#PrimarySmallButton {
    background-color: #2563eb;
    color: #ffffff;
    border: 1px solid #3b82f6;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 12px;
    font-weight: 600;
}

#PrimarySmallButton:hover {
    background-color: #1d4ed8;
}

#OutlineButton {
    background-color: #121620;
    color: #93c5fd;
    border: 1px solid #2563eb;
    border-radius: 6px;
    padding: 6px 14px;
}

#OutlineButton:hover {
    background-color: #172554;
}

#OutlineSmallButton {
    background-color: #141824;
    color: #cbd5e1;
    border: 1px solid #262f45;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 12px;
}

#OutlineSmallButton:hover {
    border-color: #60a5fa;
    color: #ffffff;
}

#EmeraldSmallButton {
    background-color: #064e3b;
    color: #6ee7b7;
    border: 1px solid #059669;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 12px;
}

#EmeraldSmallButton:hover {
    background-color: #047857;
    color: #ffffff;
}

#DangerButton {
    background-color: #dc2626;
    color: #ffffff;
    border: 1px solid #ef4444;
    border-radius: 6px;
    padding: 7px 16px;
}

#DangerButton:hover {
    background-color: #b91c1c;
}

#DangerOutlineButton {
    background-color: transparent;
    color: #f87171;
    border: 1px solid #dc2626;
    border-radius: 6px;
    padding: 6px 12px;
}

#DangerOutlineButton:hover {
    background-color: #450a0a;
}

QScrollBar:vertical {
    background: #0b0d12;
    width: 8px;
    margin: 0px;
}

QScrollBar::handle:vertical {
    background: #262f45;
    min-height: 24px;
    border-radius: 4px;
}

QScrollBar::handle:vertical:hover {
    background: #3b82f6;
}

QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
}

QTabWidget::pane {
    border: none;
    background: transparent;
}

QTabBar::tab {
    background: #121620;
    color: #94a3b8;
    border: 1px solid #1e2536;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 8px 16px;
    margin-right: 4px;
    font-weight: 600;
}

QTabBar::tab:selected {
    background: #1e293b;
    color: #60a5fa;
    border-bottom: 2px solid #3b82f6;
}
)";

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setStyleSheet(QString::fromUtf8(ANTIGRAVITY_QSS));

    MainWindow win;
    win.show();
    return app.exec();
}
