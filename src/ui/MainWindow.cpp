#include "ui/MainWindow.h"

#include "ui/shell/shell_model.h"
#include "ui/shell/top_bar.h"
#include "ui/shell/alarm_banner.h"
#include "ui/shell/nav_panel.h"
#include "ui/shell/action_bar.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"

#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QLabel>

namespace hlm {

namespace {
// Stub page: placeholder for Tasks 11-17. Shows the page name centered.
QWidget *makeStubPage(const QString &title)
{
    auto *w = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->addStretch();
    auto *label = new QLabel(title, w);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    layout->addStretch();
    return w;
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_model(new ShellModel(this))
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("PLC 调宽上位机"));
    // 1920x1080 baseline; layouts adapt to smaller/larger (spec §11.1).
    resize(1920, 1080);

    buildLayout();
    createStubPages();

    connect(m_nav, &NavPanel::pageSelected, this, &MainWindow::setCurrentPage);
    connect(m_actions, &ActionBar::actionRequested, this,
            &MainWindow::commandRequested);
    connect(m_actions, &ActionBar::loginLogoutRequested, this,
            &MainWindow::loginLogoutRequested);
}

MainWindow::~MainWindow()
{
    // App exit: clear all hold intents (spec §10.7). HoldButtons also emit
    // on destruction; this covers the owner-registered path first.
    clearHoldIntents();
}

void MainWindow::buildLayout()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_topBar = new TopBar(*m_model, central);
    m_alarmBanner = new AlarmBanner(*m_model, central);
    root->addWidget(m_topBar);
    root->addWidget(m_alarmBanner);

    auto *body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    m_nav = new NavPanel(central);
    m_pages = new QStackedWidget(central);
    m_actions = new ActionBar(*m_model, central);

    body->addWidget(m_nav);
    body->addWidget(m_pages, /*stretch=*/1);
    body->addWidget(m_actions);

    root->addLayout(body, /*stretch=*/1);
    setCentralWidget(central);
}

void MainWindow::createStubPages()
{
    // Order must match NavPanel's 7 items (spec §11.1, §11.3).
    const QStringList pages = {
        QStringLiteral("总览"),
        QStringLiteral("配方与调宽"),
        QStringLiteral("手动控制"),
        QStringLiteral("报警"),
        QStringLiteral("操作记录"),
        QStringLiteral("I/O 与诊断"),
        QStringLiteral("用户与设置"),
    };
    for (const QString &title : pages)
        m_pages->addWidget(makeStubPage(title));
}

int MainWindow::navItemCount() const
{
    return m_nav->itemCount();
}

int MainWindow::currentPageIndex() const
{
    return m_pages->currentIndex();
}

QString MainWindow::topBarText() const
{
    return m_topBar->text();
}

QString MainWindow::alarmBannerText() const
{
    return m_alarmBanner->text();
}

int MainWindow::navItemMinimumHeight() const
{
    return m_nav->itemMinimumHeight();
}

QPushButton *MainWindow::estopButton() const
{
    return static_cast<QPushButton *>(m_actions->estopButton());
}

QPushButton *MainWindow::startButton() const
{
    return static_cast<QPushButton *>(m_actions->startButton());
}

QPushButton *MainWindow::stopButton() const
{
    return static_cast<QPushButton *>(m_actions->stopButton());
}

QPushButton *MainWindow::resetButton() const
{
    return static_cast<QPushButton *>(m_actions->resetButton());
}

void MainWindow::registerHoldWidget(HoldButton *button)
{
    if (button && !m_holdWidgets.contains(button))
        m_holdWidgets.append(button);
}

void MainWindow::clearHoldIntents()
{
    for (HoldButton *b : qAsConst(m_holdWidgets))
        b->cancelHold();
}

bool MainWindow::hasActiveHolds() const
{
    for (const HoldButton *b : m_holdWidgets)
        if (b->isHeld())
            return true;
    return false;
}

void MainWindow::setCurrentPage(int index)
{
    // Page switch clears continuous-command intents (spec §10.7).
    clearHoldIntents();
    m_nav->setCurrent(index);
    m_pages->setCurrentIndex(index);
}

} // namespace hlm