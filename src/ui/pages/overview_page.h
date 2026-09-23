#pragma once

#include <QWidget>
#include <QVector>
#include <QPair>

#include "domain/barcode_result.h"
#include "ui/pages/overview_model.h"
#include "ui/widgets/status_light.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace hlm {

class ValueDisplay;
class PermissionButton;
class ShellModel;

// 总览 page (spec §11.3): 关键状态、设备示意、D120 当前步骤、目标/当前宽度、
// 差值、皮带速度、累计产量、最新报警.
//
// Display-only for machine state: it renders exclusively from
// OverviewModel/ShellModel — every stateChanged() re-renders ALL fields from
// the current snapshot (full-snapshot update, no per-field or optimistic
// updates, spec §9). Stale/invalid fields show "—" (spec §9, §11.2).
//
// 扫码服务块 (user decision 2026-09-23): the page additionally carries the scan
// service controls — a manual 采集条码 trigger, the vendor-library path and the
// forward-program path. These emit INTENT signals only; none of them is a
// machine command and none of them writes through Modbus. The page still
// declares no machine-command signal, and interacting with it still produces no
// ControlCoordinator command (asserted by the existing page tests).
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

    // --- 扫码服务块 inspection API (user decision 2026-09-23) -----------------
    PermissionButton *scanTriggerButton() const { return m_scanTrigger; }
    QString scanTriggerReasonText() const;
    QLineEdit *sdkPathEdit() const { return m_sdkPathEdit; }
    PermissionButton *saveSdkPathButton() const { return m_saveSdkPath; }
    QString sdkPathStatusText() const;
    QLineEdit *forwardExePathEdit() const { return m_forwardExeEdit; }
    PermissionButton *saveForwardExePathButton() const { return m_saveForwardExe; }
    QString forwardExePathStatusText() const;

public slots:
    // Re-renders every field from the model's current snapshot.
    void refresh();
    // The configured result path (empty = 未配置) and the terminal outcome of
    // one scan-cycle read (user decision 2026-09-22). Display-only: the
    // scanning program is the authoritative peer, so the value shown is always
    // a readback, never an optimistic success.
    void setBarcodeResultPath(const QString &path);
    void setBarcodeResult(const BarcodeResult &result);
    // Persisted deployment paths, echoed from the composition root (user
    // decision 2026-09-23). Empty means "load by name" / "do not forward".
    void setSdkPath(const QString &path);
    void setForwardExePath(const QString &path);
    // Page-local save handshakes, mirroring the settings page's result-path
    // editor: never silent, and never optimistic.
    void setSdkPathSavePending();
    void setSdkPathSaveResult(bool ok, const QString &detail);
    void setForwardExePathSavePending();
    void setForwardExePathSaveResult(bool ok, const QString &detail);

signals:
    // 扫码服务 intents for the composition root (user decision 2026-09-23).
    // None of these is a machine command: the scan cycle is driven through
    // IBarcodeSource and the two paths are persisted settings.
    void scanTriggerRequested();
    void sdkPathSaveRequested(const QString &path);
    void forwardExePathSaveRequested(const QString &path);

private:
    void buildLayout();
    QWidget *buildScanServiceBlock();
    QWidget *addField(const QString &key, const QString &title);
    // Composes the 条码/扫码 line from the configured path, M11 and the last
    // readback (never claims a connection).
    QString barcodeStatusText() const;
    // Why a 扫码服务 control is unavailable: empty = available. Only two honest
    // reasons exist — no permission, or the operation is already in flight.
    QString scanControlReasonText(bool pending) const;
    void onSaveSdkPathClicked();
    void onSaveForwardExePathClicked();

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

    // --- 扫码服务块 (user decision 2026-09-23) --------------------------------
    PermissionButton *m_scanTrigger = nullptr;
    QLabel *m_scanTriggerReason = nullptr;
    QLineEdit *m_sdkPathEdit = nullptr;
    PermissionButton *m_saveSdkPath = nullptr;
    QLabel *m_sdkPathStatus = nullptr;
    bool m_sdkPathSavePending = false;
    QLineEdit *m_forwardExeEdit = nullptr;
    PermissionButton *m_saveForwardExe = nullptr;
    QLabel *m_forwardExePathStatus = nullptr;
    bool m_forwardExePathSavePending = false;
};

} // namespace hlm
