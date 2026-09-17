// PLC-HMI-007 black-box unit tests: the parameter editors no longer seed the
// superseded specification defaults; a fresh snapshot drives the editors; no
// write request is emitted without an explicit administrator action (brief
// OB-4).
//
// Authored only from .ai/test-briefs/PLC-HMI-007.yaml (brief_version 2), the
// approved .ai/project-contract.yaml (F-10: D122=5000, D204=128, D220 effective
// 15, independent D204 1..32767 / D220 1..15 validation, removal of the
// obsolete product gate) and inspectable test sources under tests/**. No
// production implementation source was read.
//
// Frozen existing surface (all confirmed by inspectable test sources under
// tests/ unless noted): UsersSettingsPage(ShellModel&), d122Spin()/d204Spin()/
// d220Spin() (S-SETTINGS of the brief), refresh(), and the confirmed
// write-request signal UsersSettingsPage::d204WriteRequested (used by
// tests/integration/plc_hmi_005_authoritative_values_test.cpp and
// tests/unit/command_feedback_pages_test.cpp). The exact d122/d220 write-request
// signal names are not individually inspectable in this sandbox, so any signal
// whose name carries "WriteRequested" is observed generically through the meta
// object; the confirmed d204WriteRequested signal is additionally spied
// explicitly.
//
// Expected RED today: the untouched editors still seed the superseded defaults
// 1000/1280/2 instead of the authoritative 5000/128/15.

#include <QtTest>

#include <QApplication>
#include <QMetaMethod>
#include <QMetaObject>
#include <QSignalSpy>
#include <QSpinBox>
#include <QString>
#include <QVector>

#include "domain/device_snapshot.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Contract F-10 / brief OB-4 authoritative decoded defaults.
constexpr int kAuthoritativeD122 = 5000;
constexpr int kAuthoritativeD204 = 128;
constexpr int kAuthoritativeD220 = 15;

// Superseded specification defaults that must never seed the editors.
constexpr int kSupersededD122 = 1000;
constexpr int kSupersededD204 = 1280;
constexpr int kSupersededD220 = 2;

// Distinct fresh-snapshot values (inside the contract ranges: D204 1..32767,
// D220 1..15) used to prove the editors render the snapshot rather than a seed.
constexpr int kSnapshotD122 = 7000;
constexpr int kSnapshotD204 = 300;
constexpr int kSnapshotD220 = 12;

DeviceSnapshotData snapshotData(int d122, int d204, int d220)
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = static_cast<quint16>(d204);
    d.widthSpeed = static_cast<quint16>(d220);
    d.beltSpeed = static_cast<quint16>(d122);
    d.heartbeat = 1;
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

// Observes every write-request signal the page declares, by name, without
// hard-coding the d122/d220 signal names (implementation-independent). Each
// matching meta signal is spied with a signature-based QSignalSpy so no
// particular name is assumed; the confirmed d204WriteRequested signal is
// additionally spied explicitly by the test.
class WriteRequestWatcher
{
public:
    ~WriteRequestWatcher()
    {
        for (QSignalSpy *spy : m_spies)
            delete spy;
    }

    void attach(UsersSettingsPage &page)
    {
        const QMetaObject *meta = page.metaObject();
        for (int i = 0; i < meta->methodCount(); ++i) {
            const QMetaMethod method = meta->method(i);
            if (method.methodType() != QMetaMethod::Signal)
                continue;
            const QString signature = QString::fromLatin1(method.methodSignature());
            if (!signature.contains(QStringLiteral("WriteRequested"), Qt::CaseInsensitive))
                continue;
            // Coded signature exactly as the SIGNAL macro builds it:
            // <parameterCount + 1 in base 36><name(types)>.
            const QByteArray coded = QByteArray::number(method.parameterCount() + 1, 36)
                + method.methodSignature();
            m_spies.append(new QSignalSpy(&page, coded.constData()));
        }
    }

    int count() const
    {
        int total = 0;
        for (QSignalSpy *spy : m_spies)
            total += spy->count();
        return total;
    }

private:
    QVector<QSignalSpy *> m_spies;
};

} // namespace

class PlcHmi007SettingsSeedTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-4: authoritative seeds without a snapshot --------------------------
    void untouchedEditorsShowAuthoritativeDefaultsWithoutASnapshot();

    // --- OB-4: a fresh snapshot drives the editors -----------------------------
    void editorsRenderFreshSnapshotValues();

    // --- OB-4: no write without an explicit administrator action ---------------
    void noParameterWriteRequestWithoutAnExplicitAdministratorAction();

    // --- OB-4 edge (verify phase): render-only seeding, never a write ----------
    void refreshWithoutAdministratorActionNeverSeedsViaAWrite();
};

// --- OB-4 ---------------------------------------------------------------------

void PlcHmi007SettingsSeedTest::untouchedEditorsShowAuthoritativeDefaultsWithoutASnapshot()
{
    ShellModel model;
    UsersSettingsPage page(model);

    QCOMPARE(page.d122Spin()->value(), kAuthoritativeD122);
    QCOMPARE(page.d204Spin()->value(), kAuthoritativeD204);
    QCOMPARE(page.d220Spin()->value(), kAuthoritativeD220);

    QVERIFY2(page.d122Spin()->value() != kSupersededD122,
             "the D122 editor still seeds the superseded default 1000");
    QVERIFY2(page.d204Spin()->value() != kSupersededD204,
             "the D204 editor still seeds the superseded default 1280");
    QVERIFY2(page.d220Spin()->value() != kSupersededD220,
             "the D220 editor still seeds the superseded default 2");
}

void PlcHmi007SettingsSeedTest::editorsRenderFreshSnapshotValues()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.show();
    QApplication::processEvents();

    model.updateSnapshot(
        DeviceSnapshot(snapshotData(kSnapshotD122, kSnapshotD204, kSnapshotD220)));
    page.refresh();
    QApplication::processEvents();

    QCOMPARE(page.d122Spin()->value(), kSnapshotD122);
    QCOMPARE(page.d204Spin()->value(), kSnapshotD204);
    QCOMPARE(page.d220Spin()->value(), kSnapshotD220);
}

void PlcHmi007SettingsSeedTest::noParameterWriteRequestWithoutAnExplicitAdministratorAction()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    WriteRequestWatcher watcher;
    watcher.attach(page);
    QSignalSpy d204WriteSpy(&page, &UsersSettingsPage::d204WriteRequested);

    // Construction alone must not write: it seeds the editors from the
    // authoritative defaults only.
    QCOMPARE(d204WriteSpy.count(), 0);
    QCOMPARE(watcher.count(), 0);

    page.show();
    QApplication::processEvents();

    // Rendering a fresh snapshot and refreshing the page is still render-only.
    model.updateSnapshot(
        DeviceSnapshot(snapshotData(kSnapshotD122, kSnapshotD204, kSnapshotD220)));
    page.refresh();
    QApplication::processEvents();

    QCOMPARE(page.d122Spin()->value(), kSnapshotD122);
    QCOMPARE(page.d204Spin()->value(), kSnapshotD204);
    QCOMPARE(page.d220Spin()->value(), kSnapshotD220);

    QCOMPARE(d204WriteSpy.count(), 0);
    QVERIFY2(watcher.count() == 0,
             "a parameter write was requested without an explicit administrator action");
}

// --- OB-4 edge (verify phase) ---------------------------------------------------

void PlcHmi007SettingsSeedTest::refreshWithoutAdministratorActionNeverSeedsViaAWrite()
{
    // Verify-phase requirement-derived case from brief OB-4 (brief_version 2):
    // "no write is emitted without explicit administrator action". The page is
    // exercised through repeated refreshes while offline, while connected, and
    // while an administrator session is present but no parameter control was
    // touched. In every state the editors keep render-only semantics: a broad
    // meta-object scan of every "WriteRequested" signal plus the explicit
    // d204WriteRequested spy must stay at zero and the editors must not drift
    // away from the authoritative seed while no fresh snapshot exists.
    ShellModel model;
    UsersSettingsPage page(model);

    WriteRequestWatcher watcher;
    watcher.attach(page);
    QSignalSpy d204WriteSpy(&page, &UsersSettingsPage::d204WriteRequested);

    page.show();
    QApplication::processEvents();

    // Offline refreshes: the untouched editors keep the authoritative defaults.
    for (int i = 0; i < 3; ++i) {
        page.refresh();
        QApplication::processEvents();
        QCOMPARE(page.d122Spin()->value(), kAuthoritativeD122);
        QCOMPARE(page.d204Spin()->value(), kAuthoritativeD204);
        QCOMPARE(page.d220Spin()->value(), kAuthoritativeD220);
    }
    QCOMPARE(d204WriteSpy.count(), 0);
    QCOMPARE(watcher.count(), 0);

    // A connected shell with administrator session but no operator edit: still
    // no request may be fabricated by showing the page.
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.refresh();
    QApplication::processEvents();
    QCOMPARE(d204WriteSpy.count(), 0);
    QVERIFY2(watcher.count() == 0,
             "showing the settings page as an administrator fabricated a parameter "
             "write request without an explicit operator action");
}

QTEST_MAIN(PlcHmi007SettingsSeedTest)
#include "plc_hmi_007_settings_seed_test.moc"
