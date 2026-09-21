#include "ui/MainWindow.h"

#include "ui/shell/shell_model.h"
#include "ui/shell/top_bar.h"
#include "ui/shell/alarm_banner.h"
#include "ui/shell/nav_panel.h"
#include "ui/shell/action_bar.h"
#include "ui/pages/overview_page.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/pages/manual_control_page.h"
#include "ui/pages/alarm_page.h"
#include "ui/pages/audit_log_page.h"
#include "ui/pages/diagnostics_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"

#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QFile>
#include <QDebug>
#include <QEvent>
#include <QScrollArea>

// Force the theme.qss resource object into the link (static lib). Must be at
// global scope so Q_INIT_RESOURCE resolves to the un-namespaced symbol.
static void initThemeResource()
{
    Q_INIT_RESOURCE(theme);
}

namespace hlm {

MainWindow::MainWindow(QWidget *parent, ShellModel *model)
    : QMainWindow(parent)
    , m_model(model ? model : new ShellModel(this))
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("PLC 调宽上位机"));
    // 1920x1080 baseline; layouts adapt to smaller/larger (spec §11.1).
    resize(1920, 1080);

    loadTheme();

    buildLayout();
    createPages();

    connect(m_nav, &NavPanel::pageSelected, this, &MainWindow::setCurrentPage);
    connect(m_actions, &ActionBar::actionRequested, this,
            &MainWindow::commandRequested);
    connect(m_actions, &ActionBar::modeSwitchRequested, this,
            &MainWindow::modeSwitchRequested);
    connect(m_actions, &ActionBar::loginLogoutRequested, this,
            &MainWindow::loginLogoutRequested);
    setCurrentPage(0);
}

void MainWindow::loadTheme()
{
    initThemeResource();
    QFile file(QStringLiteral(":/theme.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "MainWindow: failed to open theme.qss:" << file.errorString();
        return;
    }
    setStyleSheet(QString::fromUtf8(file.readAll()));
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
    central->setObjectName(QStringLiteral("appSurface"));
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

void MainWindow::createPages()
{
    // Order must match NavPanel's 7 items (spec §11.1, §11.3). Each page lives
    // inside a widget-resizable QScrollArea so a page taller/wider than the
    // presented envelope scrolls instead of forcing a minimum window size
    // (F-02/NF-01 responsive envelope, PLC-HMI-006 D1/D2). Pages must not
    // become children of the scroll viewport: page tests instantiate them
    // standalone and every page keeps its own layout.
    m_pages->addWidget(wrapPageInScrollArea(new OverviewPage(*m_model, this)));
    m_pages->addWidget(wrapPageInScrollArea(new RecipeWidthPage(*m_model, this)));
    auto *manualPage = new ManualControlPage(*m_model, this);
    m_pages->addWidget(wrapPageInScrollArea(manualPage));
    // Register the page's HoldButtons so page switch / modal dialog / logout /
    // window deactivation cancel active holds (spec §10.7).
    for (HoldButton *hb : manualPage->findChildren<HoldButton *>())
        registerHoldWidget(hb);
    m_pages->addWidget(wrapPageInScrollArea(new AlarmPage(this)));
    m_pages->addWidget(wrapPageInScrollArea(new AuditLogPage(this)));
    m_pages->addWidget(wrapPageInScrollArea(new DiagnosticsPage(*m_model, this)));
    m_pages->addWidget(wrapPageInScrollArea(new UsersSettingsPage(*m_model, this)));
}

QScrollArea *MainWindow::wrapPageInScrollArea(QWidget *page)
{
    auto *area = new QScrollArea(this);
    area->setObjectName(QStringLiteral("pageScroll"));
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setWidget(page);
    return area;
}

QWidget *MainWindow::pageWidget(int index) const
{
    if (index < 0 || index >= m_pages->count())
        return nullptr;
    auto *area = qobject_cast<QScrollArea *>(m_pages->widget(index));
    return area != nullptr ? area->widget() : m_pages->widget(index);
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

QString MainWindow::currentPageTitle() const
{
    return m_topBar->pageTitle();
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

QPushButton *MainWindow::homeStartButton() const
{
    return static_cast<QPushButton *>(m_actions->homeStartButton());
}

void MainWindow::registerHoldWidget(HoldButton *button)
{
    if (button && !m_holdWidgets.contains(button))
        m_holdWidgets.append(button);
}

void MainWindow::clearHoldIntents()
{
    for (const QPointer<HoldButton> &b : std::as_const(m_holdWidgets))
        if (b)
            b->cancelHold();
}

bool MainWindow::hasActiveHolds() const
{
    for (const QPointer<HoldButton> &b : m_holdWidgets)
        if (b && b->isHeld())
            return true;
    return false;
}

bool MainWindow::event(QEvent *event)
{
    // QEvent::WindowDeactivate is only delivered to the top-level widget, so a
    // HoldButton on a page inside this shell never sees it. Handle it here so
    // the §10.7 窗口失活 release path works in the real app.
    if (event->type() == QEvent::WindowDeactivate)
        clearHoldIntents();
    return QMainWindow::event(event);
}

void MainWindow::setCurrentPage(int index)
{
    if (index < 0 || index >= m_pages->count())
        return;
    // Page switch clears continuous-command intents (spec §10.7).
    clearHoldIntents();
    m_nav->setCurrent(index);
    m_pages->setCurrentIndex(index);
    m_topBar->setPageTitle(m_nav->pageTitle(index));
}

} // namespace hlm
