#pragma once

#include <QWidget>

#include "domain/barcode_result.h"

class QLabel;
class QLineEdit;

namespace hlm {

class PermissionButton;
class ShellModel;

// 扫码服务 page (user decision 2026-09-23: "条码服务要单独做一栏 … 总体要单独为
// 一页"). The scan is automatic — the PLC's M15 扫码结束 edge starts a cycle with
// nobody touching the screen — and this page is where it is configured and
// watched:
//
//   - 本轮条码: every rectangle of the last cycle, position by position. An
//     empty position is shown as such, never filled with an older barcode.
//   - 采集条码: a bench control that starts the same cycle without the PLC. The
//     supplied PLC program has no M15 rung yet, so this is how the path is
//     proven on a bench; it is not part of the production flow.
//   - 扫码程序路径 / 外发程序路径: the two persisted deployment paths.
//
// The page never talks to the scanner itself: it emits intents and renders what
// the composition root feeds back, so no QProcess, no file I/O and no blocking
// wait ever runs here (contract: no scanner calls in QWidget code).
class ScanServicePage : public QWidget
{
    Q_OBJECT

public:
    explicit ScanServicePage(ShellModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    QLabel *statusLabel() const { return m_status; }
    QLabel *decodedRowsLabel() const { return m_rows; }
    QString statusText() const;
    QString decodedRowsText() const;
    QLabel *resultPathEchoLabel() const { return m_resultPathEcho; }
    PermissionButton *collectButton() const { return m_collect; }
    QString collectReasonText() const;
    QLineEdit *scanProgramEdit() const { return m_scanProgramEdit; }
    PermissionButton *saveScanProgramButton() const { return m_saveScanProgram; }
    QString scanProgramStatusText() const;
    QLineEdit *forwardProgramEdit() const { return m_forwardProgramEdit; }
    PermissionButton *saveForwardProgramButton() const { return m_saveForwardProgram; }
    QString forwardProgramStatusText() const;

public slots:
    // Re-renders from the shell model (role, scan-in-progress, M11).
    void refresh();
    // The configured result-file path, echoed for the operator. Empty means the
    // cycle still runs but nothing is written — the page says so rather than
    // letting the operator assume a file is being kept.
    void setResultPath(const QString &path);
    // Terminal outcome of one cycle.
    void setBarcodeResult(const BarcodeResult &result);
    // Persisted deployment paths, echoed from the composition root.
    void setScanProgramPath(const QString &path);
    void setForwardProgramPath(const QString &path);
    // Page-local save handshakes, mirroring the settings page's result-path
    // editor: never silent, never optimistic.
    void setScanProgramSavePending();
    void setScanProgramSaveResult(bool ok, const QString &detail);
    void setForwardProgramSavePending();
    void setForwardProgramSaveResult(bool ok, const QString &detail);

signals:
    // Intents for the composition root. None is a machine command: the scan runs
    // through IBarcodeSource and the two paths are persisted settings.
    void collectRequested();
    void scanProgramSaveRequested(const QString &path);
    void forwardProgramSaveRequested(const QString &path);

private:
    void buildLayout();
    QWidget *buildSection(const QString &title);
    // Why a control is unavailable: empty = available. Only two honest reasons
    // exist — no permission, or the operation is already in flight.
    QString controlReasonText(bool pending) const;
    void onCollectClicked();
    void onSaveScanProgramClicked();
    void onSaveForwardProgramClicked();

    ShellModel &m_model;

    QLabel *m_status = nullptr;
    QLabel *m_rows = nullptr;
    QLabel *m_resultPathEcho = nullptr;
    PermissionButton *m_collect = nullptr;
    QLabel *m_collectReason = nullptr;
    QLineEdit *m_scanProgramEdit = nullptr;
    PermissionButton *m_saveScanProgram = nullptr;
    QLabel *m_scanProgramStatus = nullptr;
    bool m_scanProgramSavePending = false;
    QLineEdit *m_forwardProgramEdit = nullptr;
    PermissionButton *m_saveForwardProgram = nullptr;
    QLabel *m_forwardProgramStatus = nullptr;
    bool m_forwardProgramSavePending = false;

    BarcodeResult m_result;
    bool m_resultSet = false;
    QString m_resultPath;
};

} // namespace hlm