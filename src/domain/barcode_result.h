#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace hlm {

// Outcome of one barcode read for the current scan cycle (user decision
// 2026-09-22). The scanning program is external: the PLC raises M15 扫码结束
// when a cycle ends and the HMI reads the result file that program writes.
//
// This value is a READBACK, never a machine command, and it is never reported
// as a success before the file has actually been read (contract: no barcode
// payload may be reported successful before confirmation from its authoritative
// peer).
enum class BarcodeState : quint8 {
    NotConfigured = 0, // no result path configured: visibly 未配置
    Ok,                // a new line was read and parsed
    // The file did not change since the last read. The scanning program keeps
    // the previous text when a read decodes nothing, so an unchanged file must
    // never be presented as this cycle's barcode (SDK README: 空读仍保留上次
    // 文件内容).
    NoNewResult,
    Failed, // the path is configured but the file is missing/unreadable/empty
};

// One barcode read result. `line` is the raw text as written by the scanning
// program, e.g. "C3003090^M10^260224^002700"; `fields` is that line split on
// '^'. The field meanings are NOT documented in the supplied reference file, so
// the UI shows the raw line and never invents a label for a field.
struct BarcodeResult {
    BarcodeState state = BarcodeState::NotConfigured;
    QString line;
    QStringList fields;
    QString detail; // operator-facing reason for NoNewResult/Failed
    QDateTime readAt;
    quint64 sequence = 0; // monotonic: lets the UI tell reads apart
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::BarcodeResult)