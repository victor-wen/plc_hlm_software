#pragma once

#include <QPushButton>
#include <QString>

namespace hlm {

// Push button that stays discoverable but disabled when the user lacks
// permission or an interlock is unmet, and explains WHY next to the action
// (tooltip + status tip) (spec §11.4: 无权限操作应保持可发现但禁用，并在
// 相邻位置或提示框说明所需权限和互锁原因).
class PermissionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit PermissionButton(QWidget *parent = nullptr);
    explicit PermissionButton(const QString &text, QWidget *parent = nullptr);

    QString disabledReason() const { return m_reason; }

public slots:
    // Single entry point used by the shell: enabled only when allowed AND
    // the widget is not otherwise disabled. When disabled, the reason is
    // shown as tooltip (adjacent hint per spec §11.4).
    void setEnabledWithReason(bool allowed, const QString &reason);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void apply();

    QString m_reason;
    bool m_allowed = false;
};

} // namespace hlm