// PLC-HMI-007 developer regression test (DISC-1): an operator edit made in the
// parameter editors must survive the stateChanged -> refresh() reentrancy and
// must be the value carried by the write request.
//
// Defect (DISC-1, introduced by the D6-d editor seeding): refresh() re-seeded
// every editor from the fresh snapshot even while the operator had already
// typed a new value, and the reentrancy happens BEFORE the handler reads
// m_d122Spin->value()/m_d204Spin->value() to emit the request. Observed:
// setValue(2560) -> setEditedD204 -> stateChanged -> refresh() -> re-seed 1280
// -> d204WriteRequested(1280). Fix: refresh() only re-renders an editor whose
// current value still equals the last value this page seeded (the baseline).
//
// This is a developer-owned test; it is not listed in .ai/test-ownership.yaml.

#include <QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>

#include "domain/device_snapshot.h"
#include "ui/dialogs/admin_password_dialog.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

constexpr int kOperatorD122 = 1500; // operator edit, inside 100-20000
constexpr int kOperatorD204 = 2560; // operator edit, inside 1-32767
constexpr int kOperatorD220 = 9;    // operator edit, inside 1-15

constexpr int kSnapshotD122 = 7000;
constexpr int kSnapshotD204 = 300;
constexpr int kSnapshotD220 = 12;

// Snapshots deliberately carry different values from the operator edits.
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

void clickAt(QWidget *w)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center,
                        w->mapToGlobal(center), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

// Finds the OK button of the re-auth dialog and confirms the password the same
// way the existing page tests do (dialog->passwordEdit() + okButton()).
bool confirmAdminPassword(UsersSettingsPage &page, const QString &password)
{
    auto *dialog = page.findChild<AdminPasswordDialog *>();
    if (!dialog)
        return false;
    dialog->passwordEdit()->setText(password);
    clickAt(dialog->okButton());
    return true;
}

} // namespace

class PlcHmi007SettingsEditPreserveTest : public QObject
{
    Q_OBJECT

private slots:
    // An operator edit survives a refresh triggered by an unrelated state
    // change, and the D122 write request carries the operator value.
    void operatorD122EditSurvivesRefreshAndIsDispatched();

    // The same preservation for D204, whose write request is emitted after the
    // re-auth dialog reentrancy.
    void operatorD204EditSurvivesRefreshAndIsDispatched();

    // Untouched editors still render the latest fresh snapshot.
    void untouchedEditorsStillRenderTheFreshSnapshot();
};

void PlcHmi007SettingsEditPreserveTest::operatorD122EditSurvivesRefreshAndIsDispatched()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(snapshotData(kSnapshotD122, kSnapshotD204,
                                                     kSnapshotD220)));
    QSignalSpy spy(&page, &UsersSettingsPage::writeParameterRequested);

    page.d122Spin()->setValue(kOperatorD122);

    // Unrelated state change: refresh() must not clobber the operator's edit.
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QApplication::processEvents();
    QCOMPARE(page.d122Spin()->value(), kOperatorD122);

    clickAt(page.writeD122Button());
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(122));
    QCOMPARE(args.at(1).toUInt(), quint16(kOperatorD122));
}

void PlcHmi007SettingsEditPreserveTest::operatorD204EditSurvivesRefreshAndIsDispatched()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(snapshotData(kSnapshotD122, kSnapshotD204,
                                                     kSnapshotD220)));
    QSignalSpy spy(&page, &UsersSettingsPage::d204WriteRequested);

    page.d204Spin()->setValue(kOperatorD204);
    clickAt(page.writeD204Button());
    QVERIFY(page.findChild<AdminPasswordDialog *>() != nullptr);

    QVERIFY2(confirmAdminPassword(page, QStringLiteral("admin-secret")),
             "re-auth dialog must expose passwordEdit()/okButton()");
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(kOperatorD204));
    QCOMPARE(args.at(1).toString(), QStringLiteral("admin-secret"));
}

void PlcHmi007SettingsEditPreserveTest::untouchedEditorsStillRenderTheFreshSnapshot()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    // No operator edit: a fresh snapshot renders into all three editors.
    model.updateSnapshot(DeviceSnapshot(snapshotData(kSnapshotD122, kSnapshotD204,
                                                     kSnapshotD220)));
    page.refresh();
    QCOMPARE(page.d122Spin()->value(), kSnapshotD122);
    QCOMPARE(page.d204Spin()->value(), kSnapshotD204);
    QCOMPARE(page.d220Spin()->value(), kSnapshotD220);

    // The operator edits only D220; a later fresh snapshot still renders into
    // the two untouched editors and leaves the edited one alone.
    page.d220Spin()->setValue(kOperatorD220);
    model.updateSnapshot(DeviceSnapshot(snapshotData(8000, 500, 7)));
    page.refresh();
    QCOMPARE(page.d122Spin()->value(), 8000);
    QCOMPARE(page.d204Spin()->value(), 500);
    QCOMPARE(page.d220Spin()->value(), kOperatorD220);
}

QTEST_MAIN(PlcHmi007SettingsEditPreserveTest)
#include "plc_hmi_007_settings_edit_preserve_developer_test.moc"
