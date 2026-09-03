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
    setMinimumHeight(40);
    setMaximumHeight(40);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 2, 12, 2);
    m_label = new QLabel(this);
    layout->addWidget(m_label);

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