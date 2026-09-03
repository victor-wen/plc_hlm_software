#pragma once

#include <QWidget>
#include <QHash>

#include "ui/pages/diagnostics_model.h"

class QLabel;
class QTableWidget;
class QVBoxLayout;

namespace hlm {

class ShellModel;
class StatusLight;
class ValueDisplay;

// I/O 与诊断 page (spec §8.2, §9, §11.3, §13): D100-D105 原始状态字、
// 已定义位 (M0-M14, M30-M45, M50-M53, M100-M112)、关键寄存器、D140 心跳
// 活性、通讯统计 (延迟/序号/重连/失败轮询) 和 OpenCV 版本与自检.
//
// Strictly read-only: the page declares NO signals and contains no command
// widgets — it can never emit a write intent (spec §11.3). 查看诊断 = 任何人
// (spec §11.4): no permission gating.
//
// Vision isolation (spec §7.4, §13): the OpenCV self-test result is fed in
// via setVisionStatus (Task 20 wires IVisionService). A failed self-test only
// marks the vision section red; PLC data display and the rest of the page stay
// normal, and no control command is ever sent.
//
// Comm statistics (延迟=最近快照 dataAgeMs、序号、重连次数、失败轮询) are fed
// in via setCommStats (Task 20 wires the gateway counters); the page only
// displays them. 延迟/序号 also derive from the current snapshot.
//
// Stale/invalid fields show "—" (spec §9). D102/D104/D105 are shown as raw
// hex only, never parsed into bits (acceptance).
class DiagnosticsPage : public QWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsPage(ShellModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    QTableWidget *rawWordTable() const { return m_rawWordTable; }
    QTableWidget *d100BitTable() const { return m_d100BitTable; }
    QTableWidget *d103BitTable() const { return m_d103BitTable; }
    QTableWidget *homeCommandTable() const { return m_homeCommandTable; }
    ValueDisplay *registerDisplay(const QString &key) const;
    ValueDisplay *commDisplay(const QString &key) const;
    StatusLight *heartbeatLight() const { return m_heartbeatLight; }
    QLabel *visionVersionLabel() const { return m_visionVersion; }
    QLabel *visionStatusLabel() const { return m_visionStatus; }
    QLabel *visionFailureLabel() const { return m_visionFailure; }

public slots:
    // Re-renders every field from the model's current snapshot.
    void refresh();
    // Vision self-test result (wired by Task 20 from IVisionService).
    void setVisionStatus(const QString &version, bool healthy,
                         const QString &failureReason);
    // Communication statistics (wired by Task 20 from the gateway).
    void setCommStats(const CommStats &stats);

private:
    void buildLayout();
    void buildRawWordTable(QVBoxLayout *root);
    void buildBitTables(QVBoxLayout *root);
    void buildRegisterGrid(QVBoxLayout *root);
    void buildCommSection(QVBoxLayout *root);
    void buildVisionSection(QVBoxLayout *root);
    void fillBitTable(QTableWidget *table, const QVector<BitRow> &rows);
    ValueDisplay *addRegister(const QString &key, const QString &title,
                              const QString &unit, QVBoxLayout *column);
    ValueDisplay *addComm(const QString &key, const QString &title,
                          const QString &unit, QVBoxLayout *column);

    ShellModel &m_model;
    DiagnosticsModel m_pageModel;

    QTableWidget *m_rawWordTable = nullptr;
    QTableWidget *m_d100BitTable = nullptr;
    QTableWidget *m_d103BitTable = nullptr;
    QTableWidget *m_homeCommandTable = nullptr;
    StatusLight *m_heartbeatLight = nullptr;
    QHash<QString, ValueDisplay *> m_registers;
    QHash<QString, ValueDisplay *> m_comm;
    QLabel *m_visionVersion = nullptr;
    QLabel *m_visionStatus = nullptr;
    QLabel *m_visionFailure = nullptr;
};

} // namespace hlm
