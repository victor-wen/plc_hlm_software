#include "ui/widgets/permission_button.h"

namespace hlm {

PermissionButton::PermissionButton(QWidget *parent)
    : QPushButton(parent)
{
    setMinimumSize(48, 48);
}

PermissionButton::PermissionButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    setMinimumSize(48, 48);
}

void PermissionButton::setEnabledWithReason(bool allowed, const QString &reason)
{
    m_allowed = allowed;
    m_reason = allowed ? QString() : reason;
    apply();
}

void PermissionButton::showEvent(QShowEvent *e)
{
    QPushButton::showEvent(e);
    apply();
}

void PermissionButton::apply()
{
    // m_allowed is the permission/interlock verdict. The widget is enabled
    // exactly when allowed; the reason is shown as tooltip + status tip
    // (spec §11.4). External disables (e.g. parent disabled) still win
    // because Qt combines them.
    setToolTip(m_reason);
    setStatusTip(m_reason);
    setEnabled(m_allowed);
}

} // namespace hlm