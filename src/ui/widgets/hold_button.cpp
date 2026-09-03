#include "ui/widgets/hold_button.h"

#include <QMouseEvent>

namespace hlm {

HoldButton::HoldButton(QWidget *parent)
    : QPushButton(parent)
{
    setMinimumSize(48, 48); // touch target (spec §11.1)
}

HoldButton::HoldButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    setMinimumSize(48, 48);
}

HoldButton::~HoldButton()
{
    // App exit while held: emit the clear so the owner can send write 0
    // (spec §10.7: 应用正常退出). Emit directly; the owner's connection
    // (e.g. coordinator logoutClear) runs during destruction.
    if (m_held) {
        m_held = false;
        emit holdChanged(false);
    }
}

void HoldButton::cancelHold()
{
    if (!m_held)
        return;
    setHeld(false);
}

void HoldButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && isEnabled() && !m_held)
        setHeld(true);
    QPushButton::mousePressEvent(e);
}

void HoldButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_held)
        setHeld(false);
    QPushButton::mouseReleaseEvent(e);
}

void HoldButton::leaveEvent(QEvent *e)
{
    // Pointer moved out while held: clear immediately (spec §10.7). The
    // later release outside must not re-press because m_held is false and
    // Qt will not deliver a press again.
    if (m_held)
        setHeld(false);
    QPushButton::leaveEvent(e);
}

bool HoldButton::event(QEvent *e)
{
    // Window deactivation (alt-tab, modal dialog taking focus): clear
    // (spec §10.7). Handled in event() because QWidget::event() does not
    // route synthetic WindowDeactivate to changeEvent().
    if (e->type() == QEvent::WindowDeactivate && m_held)
        setHeld(false);
    return QPushButton::event(e);
}

void HoldButton::setHeld(bool held)
{
    if (m_held == held)
        return;
    m_held = held;
    setDown(m_held);
    emit holdChanged(m_held);
}

QSize HoldButton::minimumSizeHint() const
{
    // Touch target >= 48x48 px (spec §11.1).
    return QSize(48, 48);
}

} // namespace hlm