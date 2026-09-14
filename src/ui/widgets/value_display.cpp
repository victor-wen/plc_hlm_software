#include "ui/widgets/value_display.h"

#include <QFontMetrics>
#include <QPainter>

namespace hlm {

namespace {

// Horizontal insets used when drawing the value. Kept as constants so the
// paint rect and minimumSizeHint() stay in sync.
constexpr int kTextLeftInset = 14;
constexpr int kTextRightInset = 10;

// Font the value is drawn with: the base font, bold, grown by 2 in whatever
// unit it uses. The theme sets `font-size: 15px`, which leaves pointSizeF()
// at -1; appending 2 to that produced a ~1pt font, so prefer the unit that is
// actually set. Used by both paintEvent() and minimumSizeHint().
QFont valueDisplayFont(const QFont &base)
{
    QFont f = base;
    f.setBold(true);
    if (f.pointSizeF() > 0.0) {
        f.setPointSizeF(f.pointSizeF() + 2.0);
    } else if (f.pixelSize() > 0) {
        f.setPixelSize(f.pixelSize() + 2);
    } else {
        f.setPointSize(11); // no size set: readable safe default
    }
    return f;
}

} // namespace

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
    p.setFont(valueDisplayFont(font()));
    p.drawText(rect().adjusted(kTextLeftInset, 0, -kTextRightInset, 0),
               Qt::AlignVCenter | Qt::AlignLeft, text());
}

QSize ValueDisplay::minimumSizeHint() const
{
    // Use the same font/insets as paintEvent so the value is not clipped at
    // the minimum width.
    const QFontMetrics fm(valueDisplayFont(font()));
    return QSize(fm.horizontalAdvance(text()) + kTextLeftInset + kTextRightInset,
                 qMax(32, fm.height()));
}

} // namespace hlm
