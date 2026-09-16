#include "ui/shell/top_bar.h"

#include "ui/shell/shell_model.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QDateTime>
#include <QPalette>
#include <QColor>
#include <QResizeEvent>

namespace hlm {

namespace {
// Horizontal padding of the wide status row (kept in sync with the grid).
constexpr int kTopBarHorizontalMargin = 8;
constexpr int kTopBarHorizontalSpacing = 10;
constexpr int kCompactLightsPerRow = 3;
} // namespace

TopBar::TopBar(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("topBar"));
    // Plain QWidget subclasses may otherwise stay transparent on Windows
    // even when QSS supplies a background color.
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumHeight(64);
    setMaximumHeight(64);

    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(kTopBarHorizontalMargin, 4, kTopBarHorizontalMargin, 4);
    m_grid->setHorizontalSpacing(kTopBarHorizontalSpacing);
    m_grid->setVerticalSpacing(2);

    m_appName = new QLabel(QStringLiteral("PLC 调宽上位机"), this);
    m_appName->setObjectName(QStringLiteral("appName"));

    m_divider = new QLabel(QStringLiteral("/"), this);
    m_divider->setObjectName(QStringLiteral("topBarDivider"));

    m_pageTitle = new QLabel(QStringLiteral("总览"), this);
    m_pageTitle->setObjectName(QStringLiteral("pageTitle"));

    buildLights();

    m_userLabel = new QLabel(this);
    m_userLabel->setObjectName(QStringLiteral("userLabel"));

    m_clockLabel = new QLabel(this);
    m_clockLabel->setObjectName(QStringLiteral("clockLabel"));

    // The row must never force the window minimum: the compact/wide
    // presentation is chosen from the available width instead (PLC-HMI-006
    // D1/D2). Relaxing the per-widget minimums keeps the permanent window
    // minimum small so the envelope stays reachable.
    for (QWidget *widget : QVector<QWidget *>{m_appName, m_divider, m_pageTitle,
                                              m_userLabel, m_clockLabel}) {
        widget->setMinimumWidth(1);
    }
    for (StatusLight *light : m_lights)
        light->setMinimumWidth(1);

    applyPresentation();

    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout, this, &TopBar::refresh);
    m_clockTimer->start();

    connect(&m_model, &ShellModel::stateChanged, this, &TopBar::refresh);
    connect(&m_model, &ShellModel::userChanged, this, &TopBar::refresh);
    refresh();
}

void TopBar::buildLights()
{
    // Order: PLC online, mode, running, homed, ready, fault/estop (spec §11.1).
    const QStringList names = {
        QStringLiteral("在线"), QStringLiteral("模式"), QStringLiteral("运行"),
        QStringLiteral("回原点"), QStringLiteral("准备"), QStringLiteral("故障/急停"),
    };
    for (const QString &name : names) {
        auto *light = new StatusLight(this);
        light->setObjectName(QStringLiteral("topStatusLight"));
        QPalette palette = light->palette();
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e7eef6")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText,
                         QColor(QStringLiteral("#aab9c7")));
        light->setPalette(palette);
        light->setState(StatusState::Unknown, name + QStringLiteral(" —"));
        m_lights.append(light);
    }
}

int TopBar::wideRowMinimumWidth() const
{
    int width = 0;
    int count = 0;
    const QVector<QWidget *> widgets = wideRowWidgets();
    for (QWidget *widget : widgets) {
        // StatusLight is a plain painted widget without a layout, so its
        // sizeHint() is invalid (-1,-1). The larger of the two hints is the
        // width the row actually needs to render its text unclipped; using
        // sizeHint() alone under-reported the row by ~670 px and let the bar
        // keep the wide row at widths where the labels were clipped.
        width += qMax(widget->sizeHint().width(), widget->minimumSizeHint().width());
        ++count;
    }
    if (count > 1)
        width += kTopBarHorizontalSpacing * (count - 1);
    width += 2 * kTopBarHorizontalMargin;
    return width;
}

QVector<QWidget *> TopBar::wideRowWidgets() const
{
    QVector<QWidget *> widgets;
    widgets.append(m_appName);
    widgets.append(m_divider);
    widgets.append(m_pageTitle);
    for (StatusLight *light : m_lights)
        widgets.append(light);
    widgets.append(m_userLabel);
    widgets.append(m_clockLabel);
    return widgets;
}

void TopBar::applyPresentation()
{
    // Wide until the status row genuinely stops fitting; then the same
    // information reflows into two light rows (never clipped/elided).
    const bool compact = width() < wideRowMinimumWidth();
    if (m_compact == compact && m_presentationPlaced)
        return;
    m_compact = compact;
    m_presentationPlaced = true;

    for (QWidget *widget : wideRowWidgets())
        m_grid->removeWidget(widget);
    for (int column = 0; column < 12; ++column)
        m_grid->setColumnStretch(column, 0);

    if (compact) {
        // Two rows: page title + clock on top, the six status lights three per
        // row below. The application name and user label are redundant with the
        // pages and the user/settings page and are dropped to keep the compact
        // bar at two/three text rows.
        setMaximumHeight(QWIDGETSIZE_MAX);
        m_appName->hide();
        m_divider->hide();
        m_userLabel->hide();

        m_grid->addWidget(m_pageTitle, 0, 0);
        m_grid->setColumnStretch(1, 1);
        m_grid->addWidget(m_clockLabel, 0, 2);
        for (int i = 0; i < m_lights.size(); ++i)
            m_grid->addWidget(m_lights[i], 1 + i / kCompactLightsPerRow,
                              i % kCompactLightsPerRow);
        m_pageTitle->show();
        m_clockLabel->show();
        for (StatusLight *light : m_lights)
            light->show();
        // The compact grid is taller than the fixed wide row: the bar must be
        // allocated at least what the two light rows need, otherwise the rows
        // overlap vertically (PLC-HMI-006 D1/D2).
        setMinimumHeight(qMax(64, m_grid->sizeHint().height()));
    } else {
        m_grid->addWidget(m_appName, 0, 0);
        m_grid->addWidget(m_divider, 0, 1);
        m_grid->addWidget(m_pageTitle, 0, 2);
        for (int i = 0; i < m_lights.size(); ++i)
            m_grid->addWidget(m_lights[i], 0, 3 + i);
        const int stretchColumn = 3 + m_lights.size();
        m_grid->setColumnStretch(stretchColumn, 1);
        m_grid->addWidget(m_userLabel, 0, stretchColumn + 1);
        m_grid->addWidget(m_clockLabel, 0, stretchColumn + 2);
        for (QWidget *widget : wideRowWidgets())
            widget->show();
        setMinimumHeight(64);
        setMaximumHeight(64);
    }
    updateGeometry();
}

void TopBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyPresentation();
}

QString TopBar::text() const
{
    QStringList parts;
    parts.append(m_appName->text());
    parts.append(m_pageTitle->text());
    for (const StatusLight *light : m_lights)
        parts.append(light->text());
    parts.append(m_userLabel->text());
    return parts.join(QLatin1Char(' '));
}

QString TopBar::pageTitle() const
{
    return m_pageTitle ? m_pageTitle->text() : QString();
}

void TopBar::setPageTitle(const QString &title)
{
    if (m_pageTitle)
        m_pageTitle->setText(title);
    // The wider title may no longer fit the current row: re-evaluate the
    // presentation instead of keeping a stale wide/compact choice until the
    // next resize (PLC-HMI-006 D1/D2).
    applyPresentation();
}

void TopBar::refresh()
{
    // 0: online
    m_lights[0]->setState(m_model.online() ? StatusState::On : StatusState::Unknown,
                          m_model.online() ? QStringLiteral("在线")
                                           : QStringLiteral("通讯中断"));

    // 1: mode — snapshot-confirmed only; unknown -> "—" (spec §11.2).
    if (!m_model.modeKnown())
        m_lights[1]->setState(StatusState::Unknown, QStringLiteral("模式 —"));
    else if (m_model.isAutoMode())
        m_lights[1]->setState(StatusState::Info, QStringLiteral("自动"));
    else
        m_lights[1]->setState(StatusState::Amber, QStringLiteral("手动"));

    // 2: running
    if (!m_model.modeKnown())
        m_lights[2]->setState(StatusState::Unknown, QStringLiteral("运行 —"));
    else if (m_model.isRunning())
        m_lights[2]->setState(StatusState::On, QStringLiteral("运行中"));
    else
        m_lights[2]->setState(StatusState::Unknown, QStringLiteral("停止"));

    // 3: homed (M61)
    if (!m_model.modeKnown())
        m_lights[3]->setState(StatusState::Unknown, QStringLiteral("回原点 —"));
    else if (m_model.isHomed())
        m_lights[3]->setState(StatusState::On, QStringLiteral("已回原点"));
    else
        m_lights[3]->setState(StatusState::Unknown, QStringLiteral("未回原点"));

    // 4: ready (M60 via D100 bit8). M8 lives in the fast block. When the state
    // is unknown show "准备 —" rather than "未准备", which would claim a
    // confirmed not-ready state (spec §9, §11.2).
    if (!m_model.modeKnown())
        m_lights[4]->setState(StatusState::Unknown, QStringLiteral("准备 —"));
    else if (m_model.snapshot().m8())
        m_lights[4]->setState(StatusState::On, QStringLiteral("准备完成"));
    else
        m_lights[4]->setState(StatusState::Unknown, QStringLiteral("未准备"));

    // 5: fault/estop
    if (m_model.isEstop())
        m_lights[5]->setState(StatusState::Error, QStringLiteral("急停"));
    else if (m_model.isFaulted())
        m_lights[5]->setState(StatusState::Error, QStringLiteral("故障"));
    else if (!m_model.modeKnown())
        m_lights[5]->setState(StatusState::Unknown, QStringLiteral("故障/急停 —"));
    else
        m_lights[5]->setState(StatusState::On, QStringLiteral("正常"));

    m_userLabel->setText(QStringLiteral("用户 · %1").arg(m_model.userName()));
    m_clockLabel->setText(QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

} // namespace hlm
