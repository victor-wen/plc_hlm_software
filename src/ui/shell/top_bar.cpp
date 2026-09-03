#include "ui/shell/top_bar.h"

#include "ui/shell/shell_model.h"

#include <QHBoxLayout>
#include <QTimer>
#include <QDateTime>

namespace hlm {

TopBar::TopBar(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("topBar"));
    setMinimumHeight(72);
    setMaximumHeight(72);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 4, 12, 4);
    layout->setSpacing(16);

    m_appName = new QLabel(QStringLiteral("PLC 调宽上位机"), this);
    m_appName->setObjectName(QStringLiteral("appName"));
    layout->addWidget(m_appName);

    buildLights();

    layout->addStretch();

    m_userLabel = new QLabel(this);
    layout->addWidget(m_userLabel);

    m_clockLabel = new QLabel(this);
    m_clockLabel->setObjectName(QStringLiteral("clockLabel"));
    layout->addWidget(m_clockLabel);

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
        light->setState(StatusState::Unknown, name + QStringLiteral(" —"));
        m_lights.append(light);
        layout()->addWidget(light);
    }
}

QString TopBar::text() const
{
    QStringList parts;
    parts.append(m_appName->text());
    for (const StatusLight *light : m_lights)
        parts.append(light->text());
    parts.append(m_userLabel->text());
    return parts.join(QLatin1Char(' '));
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

    // 4: ready (M60 via D100 bit8)
    const bool ready = m_model.snapshotFresh() && m_model.snapshot().m8();
    m_lights[4]->setState(ready ? StatusState::On : StatusState::Unknown,
                          ready ? QStringLiteral("准备完成")
                                : QStringLiteral("未准备"));

    // 5: fault/estop
    if (m_model.isEstop())
        m_lights[5]->setState(StatusState::Error, QStringLiteral("急停"));
    else if (m_model.isFaulted())
        m_lights[5]->setState(StatusState::Error, QStringLiteral("故障"));
    else if (!m_model.modeKnown())
        m_lights[5]->setState(StatusState::Unknown, QStringLiteral("故障/急停 —"));
    else
        m_lights[5]->setState(StatusState::On, QStringLiteral("正常"));

    m_userLabel->setText(m_model.userName().isEmpty()
                             ? QStringLiteral("未登录")
                             : m_model.userName());
    m_clockLabel->setText(QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

} // namespace hlm