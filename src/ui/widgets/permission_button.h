#pragma once

#include <QPushButton>
#include <QString>

class QLabel;

namespace hlm {

// Push button that stays discoverable but disabled when the user lacks
// permission or an interlock is unmet, and explains WHY next to the action.
//
// The exact reason is rendered as inline visible text on the control itself
// (touch-accessible without hover; contract OperatorCommandStatus
// presentation.disabled_reasons, PLC-HMI-008 D3) and remains available as
// tooltip + status tip supplement (spec §11.4: 无权限操作应保持可发现但禁用，
// 并在相邻位置或提示框说明所需权限和互锁原因).
class PermissionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit PermissionButton(QWidget *parent = nullptr);
    explicit PermissionButton(const QString &text, QWidget *parent = nullptr);

    QString disabledReason() const { return m_reason; }
    // Exact inline reason text shown while the control is disabled; empty
    // while the control is allowed.
    QString visibleReasonText() const;
    // The inline reason label (child of this control, laid out at the bottom).
    QLabel *reasonLabel() const { return m_reasonLabel; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    // Single entry point used by the shell: enabled only when allowed AND
    // the widget is not otherwise disabled. When disabled, the reason is
    // rendered as inline visible text (never tooltip-only).
    void setEnabledWithReason(bool allowed, const QString &reason);

protected:
    void showEvent(QShowEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void apply();
    // Keeps the bottom-aligned reason to its single reserved line below the
    // top-painted title (user decision 2026-09-24: the height never follows the
    // reason text, so the control keeps a fixed size).
    void clampReasonHeight();

    QString m_reason;
    bool m_allowed = false;
    QLabel *m_reasonLabel = nullptr;
};

} // namespace hlm
