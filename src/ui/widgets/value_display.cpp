#include "ui/widgets/value_display.h"

#include <QPainter>

namespace hlm {

ValueDisplay::ValueDisplay(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(32);
}

void ValueDisplay::setValue(const QString &value, const QString &unit, bool valid)
{
    m_value = value;
    m_unit = unit;
    m_valid = valid;
    update();
}

QString ValueDisplay::text() const
{
    if (!m_valid)
        return QStringLiteral("—");
    return m_unit.isEmpty() ? m_value : m_value + QLatin1Char(' ') + m_unit;
}

void ValueDisplay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // Invalid/stale: grey + "—" (spec §11.2: 离线/未知/过期 -> 灰色).
    p.setPen(m_valid ? palette().color(QPalette::WindowText)
                     : palette().color(QPalette::Disabled, QPalette::WindowText));
    p.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, text());
}

QSize ValueDisplay::minimumSizeHint() const
{
    return QSize(fontMetrics().horizontalAdvance(text()) + 8, 32);
}

} // namespace hlm