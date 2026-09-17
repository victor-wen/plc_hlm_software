// PLC-HMI-007 black-box unit tests: the diagnostics surface shows the real
// data age (never a fabricated zero), an invalid marker for stale/absent data,
// and a visible legible D138/D139 count-reliability disclosure (brief OB-5).
//
// Authored only from .ai/test-briefs/PLC-HMI-007.yaml (brief_version 2), the
// approved .ai/project-contract.yaml (NF-07, ARCH-008, ARCH-014, F-08 and the
// DeviceSnapshot quality rules: real transfer ages, no hard-coded zero, stale
// data is not presented as fresh) and inspectable test sources under tests/**.
// No production implementation source was read.
//
// Frozen existing surface (brief S-DIAG): DiagnosticsPage(ShellModel&),
// setCommStats(CommStats), commDisplay(key) returning a ValueDisplay*, keys
// including "latency". ValueDisplay exposes the public accessor
// `QString text() const`, used by the existing tests/unit/test_diagnostics_page.cpp
// pattern `page.commDisplay(...)->text()`, so the rendered text is read through
// that public accessor only. The UI-side hlm::CommStats value has exactly the
// observable members sequence, lastDataAgeMs, reconnectCount and failedPolls;
// no other member is referenced. The latency display renders the snapshot's
// overall data age (max of the block ages) and "—" when the data is invalid.
//
// Expected RED today: no real fast-block age reaches the latency display (a
// fabricated zero is shown) and the reliability disclosure for the D138/D139
// production count is absent from the diagnostics surface. The disclosure case
// is a regression lock if the diagnostics surface already renders it.
//
// Verify phase added the requirement-derived edge case
// freshLatencyFollowsTheCurrentDataAgeWithoutStaleOrFabricatedValues (OB-5):
// the displayed age must follow each current data age (distinct real values,
// never a fabricated zero), and after a stale phase a fresh snapshot must
// again render its own new real value.

#include <QtTest>

#include <QApplication>
#include <QLabel>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "domain/device_snapshot.h"
#include "ui/pages/diagnostics_page.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/value_display.h"

using namespace hlm;

namespace {

constexpr int kFastAgeMs = 1234;
constexpr int kSecondFreshAgeMs = 4500;
constexpr int kOtherBlockAgeMs = 0;
constexpr int kStaleAgeMs = 9000;

// The 005 test file's legible touch-text minimum; the brief requires a visible,
// non-hidden, legible disclosure.
constexpr int kMinimumDisclosureFontHeight = 16;

DeviceSnapshotData snapshotData(qint64 fastAge, DataQuality fastQuality)
{
    DeviceSnapshotData d;
    d.connected = fastQuality == DataQuality::Valid;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 128;
    d.widthSpeed = 15;
    d.beltSpeed = 5000;
    d.heartbeat = 1;
    d.fast_age_ms = fastAge;
    d.home_age_ms = kOtherBlockAgeMs;
    d.command_age_ms = kOtherBlockAgeMs;
    d.slow_age_ms = kOtherBlockAgeMs;
    d.fast_quality = fastQuality;
    d.home_quality = fastQuality == DataQuality::Valid ? DataQuality::Valid
                                                       : DataQuality::Stale;
    d.command_quality = fastQuality == DataQuality::Valid ? DataQuality::Valid
                                                          : DataQuality::Stale;
    d.slow_quality = fastQuality == DataQuality::Valid ? DataQuality::Valid
                                                       : DataQuality::Stale;
    d.overall_quality = aggregateQuality(d);
    return d;
}

// The diagnostics page consumes the UI-side communication-statistics value
// (hlm::CommStats, brief S-DIAG). Its complete observable surface is
// sequence, lastDataAgeMs, reconnectCount and failedPolls.
CommStats commStats(qint64 perBlockAgeMs)
{
    CommStats stats{};
    stats.sequence = 1;
    stats.lastDataAgeMs = perBlockAgeMs;
    stats.reconnectCount = 0;
    stats.failedPolls = 0;
    return stats;
}

bool containsInvalidMarker(const QString &text)
{
    // The brief fixes the invalid representation as an em dash; an en dash or
    // the ASCII hyphen are accepted as the same observable invalid marker.
    return text.contains(QChar(0x2014)) || text.contains(QChar(0x2013))
        || text == QStringLiteral("-");
}

bool containsReliabilityWording(const QString &text)
{
    const QStringList tokens{
        QStringLiteral("不可靠"), QStringLiteral("可能不"),
        QStringLiteral("失真"),   QStringLiteral("不保证"),
        QStringLiteral("仅供参考"), QStringLiteral("不准确"),
        QStringLiteral("偏差"),   QStringLiteral("异常"),
    };
    for (const QString &token : tokens) {
        if (text.contains(token))
            return true;
    }
    return false;
}

bool mentionsProductionCount(const QString &text)
{
    const QStringList tokens{QStringLiteral("产量"), QStringLiteral("生产"),
                             QStringLiteral("计数"), QStringLiteral("D138"),
                             QStringLiteral("D139")};
    for (const QString &token : tokens) {
        if (text.contains(token))
            return true;
    }
    return false;
}

} // namespace

class PlcHmi007DiagnosticsSurfaceTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-5: real data age, never a fabricated zero ---------------------------
    void freshLatencyShowsTheRealFastBlockAgeAndNeverAFabricatedZero();

    // --- OB-5: stale data renders the invalid marker ----------------------------
    void staleLatencyRendersTheInvalidMarkerInsteadOfARealAge();

    // --- OB-5: visible legible D138/D139 reliability disclosure -----------------
    void productionCountReliabilityDisclosureIsVisibleAndLegible();

    // --- OB-5 edge (verify phase): the age tracks the current data ---------------
    void freshLatencyFollowsTheCurrentDataAgeWithoutStaleOrFabricatedValues();
};

// --- OB-5 ---------------------------------------------------------------------

void PlcHmi007DiagnosticsSurfaceTest::freshLatencyShowsTheRealFastBlockAgeAndNeverAFabricatedZero()
{
    ShellModel model;
    DiagnosticsPage page(model);
    page.resize(1920, 1080);
    page.show();
    QApplication::processEvents();

    model.updateSnapshot(DeviceSnapshot(snapshotData(kFastAgeMs, DataQuality::Valid)));
    page.setCommStats(commStats(kFastAgeMs));
    page.refresh();
    QApplication::processEvents();

    ValueDisplay *latency = page.commDisplay(QStringLiteral("latency"));
    QVERIFY2(latency != nullptr,
             "the diagnostics page has no comm display for the 'latency' key");

    const QString text = latency->text().trimmed();
    QVERIFY2(text.contains(QStringLiteral("1234")),
             qPrintable(QStringLiteral(
                            "the latency display does not present the real 1234 ms "
                            "block age; rendered text: '%1'")
                            .arg(text)));

    // A fabricated zero: exactly "0", "0 ms", or a unit that carries no
    // non-zero digit at all.
    QVERIFY2(!QRegularExpression(QStringLiteral("(^|[^0-9])0\\s*(ms|毫秒)?\\s*$"))
                  .match(text)
                  .hasMatch(),
             qPrintable(QStringLiteral(
                            "the latency display presents a fabricated zero; "
                            "rendered text: '%1'")
                            .arg(text)));
}

void PlcHmi007DiagnosticsSurfaceTest::staleLatencyRendersTheInvalidMarkerInsteadOfARealAge()
{
    ShellModel model;
    DiagnosticsPage page(model);
    page.resize(1920, 1080);
    page.show();
    QApplication::processEvents();

    // Precondition: fresh data renders the real age first.
    model.updateSnapshot(DeviceSnapshot(snapshotData(kFastAgeMs, DataQuality::Valid)));
    page.setCommStats(commStats(kFastAgeMs));
    page.refresh();
    QApplication::processEvents();

    ValueDisplay *latency = page.commDisplay(QStringLiteral("latency"));
    QVERIFY2(latency != nullptr,
             "the diagnostics page has no comm display for the 'latency' key");
    {
        const QString freshText = latency->text().trimmed();
        QVERIFY2(freshText.contains(QStringLiteral("1234")),
                 qPrintable(QStringLiteral(
                                "precondition: the latency display does not show the "
                                "fresh 1234 ms age; rendered text: '%1'")
                                .arg(freshText)));
    }

    // Stale data: the invalid marker must replace the number.
    model.updateSnapshot(DeviceSnapshot(snapshotData(kStaleAgeMs, DataQuality::Stale)));
    page.setCommStats(commStats(kStaleAgeMs));
    page.refresh();
    QApplication::processEvents();

    const QString text = latency->text().trimmed();
    QVERIFY2(!text.contains(QStringLiteral("1234")),
             qPrintable(QStringLiteral(
                            "the stale latency display still presents the previous "
                            "fresh age; rendered text: '%1'")
                            .arg(text)));
    QVERIFY2(!text.contains(QStringLiteral("9000")),
             qPrintable(QStringLiteral(
                            "the stale latency display presents the stale age as a "
                            "real value; rendered text: '%1'")
                            .arg(text)));
    QVERIFY2(containsInvalidMarker(text),
             qPrintable(QStringLiteral(
                            "the stale latency display does not render the invalid "
                            "marker; rendered text: '%1'")
                            .arg(text)));
}

void PlcHmi007DiagnosticsSurfaceTest::productionCountReliabilityDisclosureIsVisibleAndLegible()
{
    ShellModel model;
    DiagnosticsPage page(model);
    page.resize(1920, 1080);
    page.show();
    QApplication::processEvents();

    model.updateSnapshot(DeviceSnapshot(snapshotData(kFastAgeMs, DataQuality::Valid)));
    page.refresh();
    QApplication::processEvents();

    QLabel *disclosure = nullptr;
    for (QLabel *label : page.findChildren<QLabel *>()) {
        const QString text = label->text().trimmed();
        if (text.isEmpty() || label->isHidden() || !label->isVisibleTo(&page))
            continue;
        if (!mentionsProductionCount(text) || !containsReliabilityWording(text))
            continue;
        disclosure = label;
        break;
    }
    QVERIFY2(disclosure != nullptr,
             "the diagnostics surface shows no visible D138/D139 production-count "
             "reliability disclosure");
    QVERIFY2(!disclosure->isHidden(),
             "the reliability disclosure label is hidden");
    QVERIFY2(disclosure->isVisibleTo(&page),
             "the reliability disclosure label is not visible on the diagnostics page");
    QVERIFY2(disclosure->fontMetrics().height() >= kMinimumDisclosureFontHeight,
             qPrintable(QStringLiteral(
                            "the reliability disclosure font height %1 is below the "
                            "legible minimum %2; text='%3'")
                            .arg(disclosure->fontMetrics().height())
                            .arg(kMinimumDisclosureFontHeight)
                            .arg(disclosure->text().trimmed())));
}

// --- OB-5 edge (verify phase) -------------------------------------------------

void PlcHmi007DiagnosticsSurfaceTest::
    freshLatencyFollowsTheCurrentDataAgeWithoutStaleOrFabricatedValues()
{
    // Verify-phase requirement-derived case from brief OB-5 (brief_version 2):
    // the diagnostics surface must show the real current data age, never a
    // fabricated or stale value. Two different fresh ages must each render their
    // own number, the display must never show a fabricated zero while the data
    // is fresh, and after a stale phase a fresh snapshot with a new age must
    // again render that new real value (recovery), not keep the old one.
    ShellModel model;
    DiagnosticsPage page(model);
    page.resize(1920, 1080);
    page.show();
    QApplication::processEvents();

    ValueDisplay *latency = page.commDisplay(QStringLiteral("latency"));
    QVERIFY2(latency != nullptr,
             "the diagnostics page has no comm display for the 'latency' key");

    // The same checker is used for every fresh phase: the rendered text must
    // carry the current real age and must not be a fabricated zero.
    const auto verifyFreshAge = [&](int ageMs, const QString &phase) {
        const QString text = latency->text().trimmed();
        QVERIFY2(text.contains(QString::number(ageMs)),
                 qPrintable(QStringLiteral(
                                "%1: the latency display does not present the current "
                                "real %2 ms data age; rendered text: '%3'")
                                .arg(phase)
                                .arg(ageMs)
                                .arg(text)));
        QVERIFY2(!QRegularExpression(QStringLiteral("(^|[^0-9])0\\s*(ms|毫秒)?\\s*$"))
                      .match(text)
                      .hasMatch(),
                 qPrintable(QStringLiteral(
                                "%1: the latency display presents a fabricated zero "
                                "although the data is fresh; rendered text: '%2'")
                                .arg(phase, text)));
    };

    // First fresh age: 1234 ms.
    model.updateSnapshot(DeviceSnapshot(snapshotData(kFastAgeMs, DataQuality::Valid)));
    page.setCommStats(commStats(kFastAgeMs));
    page.refresh();
    QApplication::processEvents();
    verifyFreshAge(kFastAgeMs, QStringLiteral("first fresh phase"));

    // Second fresh age: 4500 ms must replace the first number.
    model.updateSnapshot(
        DeviceSnapshot(snapshotData(kSecondFreshAgeMs, DataQuality::Valid)));
    page.setCommStats(commStats(kSecondFreshAgeMs));
    page.refresh();
    QApplication::processEvents();
    {
        const QString text = latency->text().trimmed();
        QVERIFY2(!text.contains(QStringLiteral("1234")),
                 qPrintable(QStringLiteral(
                                "the latency display still presents the previous "
                                "1234 ms age after newer data arrived; rendered "
                                "text: '%1'")
                                .arg(text)));
    }
    verifyFreshAge(kSecondFreshAgeMs, QStringLiteral("second fresh phase"));

    // Stale phase: no real age may be shown.
    model.updateSnapshot(DeviceSnapshot(snapshotData(kStaleAgeMs, DataQuality::Stale)));
    page.setCommStats(commStats(kStaleAgeMs));
    page.refresh();
    QApplication::processEvents();
    {
        const QString staleText = latency->text().trimmed();
        QVERIFY2(!staleText.contains(QStringLiteral("4500")),
                 qPrintable(QStringLiteral(
                                "the stale latency display presents the previous "
                                "4500 ms age as a real value; rendered text: '%1'")
                                .arg(staleText)));
        QVERIFY2(containsInvalidMarker(staleText),
                 qPrintable(QStringLiteral(
                                "the stale latency display does not render the invalid "
                                "marker; rendered text: '%1'")
                                .arg(staleText)));
    }

    // Recovery: a fresh snapshot with yet another age must render that new real
    // value again, never a fabricated zero and never the stale phase's absence.
    model.updateSnapshot(DeviceSnapshot(snapshotData(kFastAgeMs, DataQuality::Valid)));
    page.setCommStats(commStats(kFastAgeMs));
    page.refresh();
    QApplication::processEvents();
    verifyFreshAge(kFastAgeMs, QStringLiteral("recovered fresh phase"));
}

QTEST_MAIN(PlcHmi007DiagnosticsSurfaceTest)
#include "plc_hmi_007_diagnostics_surface_test.moc"
