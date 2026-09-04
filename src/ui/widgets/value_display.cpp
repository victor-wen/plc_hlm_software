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
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(m_valid ? QColor(0xd7, 0xe0, 0xea)
                          : QColor(0xe2, 0xe8, 0xf0), 1));
    p.setBrush(m_valid ? QColor(0xff, 0xff, 0xff)
                       : QColor(0xf1, 0xf5, 0xf9));
    p.drawRoundedRect(box, 7, 7);

    // Invalid/stale: grey + "—" (spec §11.2: 离线/未知/过期 -> 灰色).
    p.setPen(m_valid ? palette().color(QPalette::WindowText)
                     : palette().color(QPalette::Disabled, QPalette::WindowText));
    QFont valueFont = font();
    valueFont.setBold(true);
    valueFont.setPointSizeF(valueFont.pointSizeF() + 2.0);
    p.setFont(valueFont);
    p.drawText(rect().adjusted(14, 0, -10, 0),
               Qt::AlignVCenter | Qt::AlignLeft, text());
}

QSize ValueDisplay::minimumSizeHint() const
{
    return QSize(fontMetrics().horizontalAdvance(text()) + 8, 32);
}

} // namespace hlm
