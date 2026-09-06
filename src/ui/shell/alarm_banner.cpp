#include "ui/shell/alarm_banner.h"

#include "ui/shell/shell_model.h"

#include <QHBoxLayout>
#include <QStyle>

namespace hlm {

AlarmBanner::AlarmBanner(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("alarmBanner"));
    // Ensure the green/red status background is painted by this QWidget
    // subclass on the Windows style engine.
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumHeight(40);
    setMaximumHeight(40);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 2, 14, 2);
    layout->setSpacing(12);
    auto *tag = new QLabel(QStringLiteral("系统状态"), this);
    tag->setObjectName(QStringLiteral("alarmBannerTag"));
    layout->addWidget(tag);
    m_label = new QLabel(this);
    m_label->setObjectName(QStringLiteral("alarmBannerText"));
    layout->addWidget(m_label);
    layout->addStretch();
    auto *source = new QLabel(QStringLiteral("PLC 实时确认"), this);
    source->setObjectName(QStringLiteral("alarmBannerSource"));
    layout->addWidget(source);

    connect(&m_model, &ShellModel::stateChanged, this, &AlarmBanner::refresh);
    refresh();
}

void AlarmBanner::refresh()
{
    const QString alarm = m_model.activeAlarmText();
    if (alarm.isEmpty()) {
        m_label->setText(QStringLiteral("无报警"));
        // Green "无报警" (spec §11.1).
        setObjectName(QStringLiteral("alarmBannerOk"));
    } else {
        m_label->setText(alarm);
        setObjectName(QStringLiteral("alarmBannerActive"));
    }
    // Repolish so the QSS objectName change takes effect.
    style()->unpolish(this);
    style()->polish(this);
}

} // namespace hlm
