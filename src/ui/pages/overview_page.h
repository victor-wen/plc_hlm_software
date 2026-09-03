#pragma once

#include <QWidget>
#include <QVector>
#include <QPair>

#include "ui/pages/overview_model.h"
#include "ui/widgets/status_light.h"

class QLabel;
class QVBoxLayout;

namespace hlm {

class ValueDisplay;
class ShellModel;

// 总览 page (spec §11.3): 关键状态、设备示意、D120 当前步骤、目标/当前宽度、
// 差值、皮带速度、累计产量、最新报警.
//
// Strictly read-only: the page declares NO signals and contains no command
// widgets — it can never emit a write intent. It renders exclusively from
// OverviewModel/ShellModel: every stateChanged() re-renders ALL fields from
// the current snapshot (full-snapshot update, no per-field or optimistic
// updates, spec §9). Stale/invalid fields show "—" (spec §9, §11.2).
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(ShellModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    ValueDisplay *fieldDisplay(const QString &key) const;
    QLabel *latestAlarmLabel() const;
    QString latestAlarmText() const;

public slots:
    // Re-renders every field from the model's current snapshot.
    void refresh();

private:
    void buildLayout();
    ValueDisplay *addField(const QString &key, const QString &title,
                           QVBoxLayout *column);

    ShellModel &m_model;
    OverviewModel m_pageModel;

    QLabel *m_alarmLabel = nullptr;
    QVector<StatusLight *> m_statusLights; // online, mode, running, fault
    QHash<QString, ValueDisplay *> m_displays;
};

} // namespace hlm