#include "ui/widgets/status_light.h"

#include <QPainter>
#include <QFontMetrics>

namespace hlm {

namespace {

QColor stateColor(StatusState state)
{
    switch (state) {
    case StatusState::On:    return QColor(0x2e, 0x9e, 0x4f); // green 在线/完成/准备
    case StatusState::Info:  return QColor(0x2f, 0x6f, 0xd0); // blue 自动/发送中
    case StatusState::Amber: return QColor(0xd8, 0xa0, 0x20); // amber 手动/警告
    case StatusState::Error: return QColor(0xc4, 0x2b, 0x2b); // red 故障/急停
    case StatusState::Unknown: break;
    }
    return QColor(0x8a, 0x8a, 0x8a); // gray 离线/未知/过期
}

} // namespace

StatusLight::StatusLight(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(24);
}

void StatusLight::setState(StatusState state, const QString &text)
{
    if (m_state == state && m_text == text)
        return;
    m_state = state;
    m_text = text;
    update();
}

void StatusLight::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int dot = fontMetrics().height();
    const int margin = 4;
    const QRectF dotRect(margin, (height() - dot) / 2.0, dot, dot);

    p.setPen(Qt::NoPen);
    p.setBrush(stateColor(m_state));
    p.drawEllipse(dotRect);

    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRectF(dotRect.right() + margin, 0,
                      width() - dotRect.right() - 2 * margin, height()),
               Qt::AlignVCenter | Qt::AlignLeft, m_text);
}

QSize StatusLight::minimumSizeHint() const
{
    const int dot = fontMetrics().height();
    return QSize(dot * 2 + fontMetrics().horizontalAdvance(m_text) + 12,
                 qMax(dot + 8, 24));
}

} // namespace hlm