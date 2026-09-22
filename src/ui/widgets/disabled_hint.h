#pragma once

#include <QString>
#include <QWidget>

namespace hlm {

// Hover hint for a control that is currently unavailable (user decision
// 2026-09-22: 按钮不可用时鼠标放上去要显示原因).
//
// Qt does deliver QEvent::ToolTip to a DISABLED widget: QWidget::event's
// disabled filter covers mouse press/move/release and wheel, but not ToolTip
// (Qt 6.4, qwidget.cpp), and QApplication::notify arms the tooltip wake-up
// timer for the widget under the cursor without consulting isEnabled(). So a
// tooltip set here really does appear on hover, as long as the top-level window
// is active.
//
// This is the hover supplement only. The contract still requires the reason to
// be readable as inline text next to the control (spec §11.4, PLC-HMI-008 D3);
// PermissionButton renders it inside itself. Never tooltip-only.
inline void setUnavailableHint(QWidget *control, bool available, const QString &reason)
{
    if (control == nullptr)
        return;
    const QString hint = available ? QString() : reason;
    control->setToolTip(hint);
    control->setStatusTip(hint);
}

} // namespace hlm