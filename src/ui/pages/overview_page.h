#pragma once

#include <QWidget>
#include <QVector>
#include <QPair>

#include "domain/barcode_result.h"
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
    // Visible reliability disclosure for the D138/D139 production count.
    QLabel *productionCountDisclosureLabel() const;
    // 条码/扫码 status line. With no result path configured it stays the
    // 未配置 placeholder and never implies a connection; with a path
    // configured it shows the last readback (user decision 2026-09-22).
    QLabel *barcodePlaceholderLabel() const;
    QString barcodeText() const;

public slots:
    // Re-renders every field from the model's current snapshot.
    void refresh();
    // The configured result path (empty = 未配置) and the terminal outcome of
    // one scan-cycle read (user decision 2026-09-22). Display-only: the
    // scanning program is the authoritative peer, so the value shown is always
    // a readback, never an optimistic success.
    void setBarcodeResultPath(const QString &path);
    void setBarcodeResult(const BarcodeResult &result);

private:
    void buildLayout();
    QWidget *addField(const QString &key, const QString &title);
    // Composes the 条码/扫码 line from the configured path, M11 and the last
    // readback (never claims a connection).
    QString barcodeStatusText() const;

    ShellModel &m_model;
    OverviewModel m_pageModel;

    QLabel *m_alarmLabel = nullptr;
    QLabel *m_productionCountLabel = nullptr;
    QLabel *m_productionDisclosure = nullptr;
    QLabel *m_barcodePlaceholder = nullptr;
    // Configured result path echoed by the settings page (empty = 未配置).
    QString m_barcodePath;
    // Latest scan readback, rendered by refresh()/setBarcodeResult().
    BarcodeResult m_barcodeResult;
    bool m_barcodeResultSet = false;
    QVector<StatusLight *> m_statusLights; // online, mode, running, fault
    QHash<QString, ValueDisplay *> m_displays;
};

} // namespace hlm
