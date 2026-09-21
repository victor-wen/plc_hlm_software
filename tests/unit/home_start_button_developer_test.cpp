// Developer-owned test for the new 回原点 (HomeStart) surface (user decision
// 2026-09-21): the ActionBar carries a dedicated 回原点 control next to
// 启动/复位, it dispatches Command::HomeStart (never Reset), it is admin-only,
// and it reports its remaining gate as visible inline text instead of a
// tooltip alone. Reset is covered by the existing operator_command_lifecycle
// cases; this target covers the presentation the operator actually touches.

#include <QtTest>

#include <QApplication>
#include <QPushButton>

#include "domain/device_snapshot.h"
#include "ui/MainWindow.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

// Manual-mode machine with valid blocks; M50 (home block bit0) is cleared.
DeviceSnapshotData machineData(bool manual = true)
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = manual ? quint16(1) << 1 : quint16(1) << 2;
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 1280;
    d.widthSpeed = 15;
    d.beltSpeed = 1000;
    d.heartbeat = 1;
    d.homeBits = 0;
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

} // namespace

class HomeStartButtonDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void actionBarExposesHomeStartAndDispatchesItsOwnCommand();
    void homeStartStaysDisabledWithoutAdmin();
    void homeStartIsDisabledWhileHomingIsInProgress();
    void resetDoesNotDispatchHomeStart();
};

void HomeStartButtonDeveloperTest::actionBarExposesHomeStartAndDispatchesItsOwnCommand()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ActionBar bar(model);
    model.updateSnapshot(DeviceSnapshot(machineData()));
    bar.resize(240, 900);
    bar.show();
    QApplication::processEvents();

    QVERIFY2(bar.homeStartButton() != nullptr,
             "the action bar must expose the 回原点 control");
    QCOMPARE(bar.homeStartButton()->text(), QStringLiteral("回原点"));

    QVector<Command> requested;
    connect(&bar, &ActionBar::actionRequested, this,
            [&requested](Command cmd) { requested.append(cmd); });
    bar.homeStartButton()->click();

    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first(), Command::HomeStart);
}

void HomeStartButtonDeveloperTest::homeStartStaysDisabledWithoutAdmin()
{
    ShellModel model;
    model.setUser(QString(), Role::Anonymous);
    ActionBar bar(model);
    model.updateSnapshot(DeviceSnapshot(machineData()));
    bar.resize(240, 900);
    bar.show();
    QApplication::processEvents();

    QVERIFY(!bar.homeStartButton()->isEnabled());
    QVERIFY2(!bar.homeStartButton()->visibleReasonText().isEmpty(),
             "an anonymous session must see why 回原点 is disabled");
    QVERIFY(bar.homeStartButton()->visibleReasonText().contains(
        QStringLiteral("管理员")));
}

void HomeStartButtonDeveloperTest::homeStartIsDisabledWhileHomingIsInProgress()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ActionBar bar(model);
    bar.resize(240, 900);
    bar.show();

    // M50 set: the machine is already homing (M50 lives in the home block).
    DeviceSnapshotData homing = machineData();
    homing.homeBits = 0x0001; // M50
    model.updateSnapshot(DeviceSnapshot(homing));
    QApplication::processEvents();

    QVERIFY(!bar.homeStartButton()->isEnabled());
    QVERIFY(bar.homeStartButton()->visibleReasonText().contains(
        QStringLiteral("正在回原点")));

    // The PLC cleared M50: the control is usable again.
    model.updateSnapshot(DeviceSnapshot(machineData()));
    QApplication::processEvents();
    QVERIFY(bar.homeStartButton()->isEnabled());
}

void HomeStartButtonDeveloperTest::resetDoesNotDispatchHomeStart()
{
    // The two are separate commands: clicking 复位 must never be routed to the
    // homing dispatcher (the composition root routes both through
    // Application::onCommandRequested).
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ActionBar bar(model);
    model.updateSnapshot(DeviceSnapshot(machineData()));
    bar.resize(240, 900);
    bar.show();
    QApplication::processEvents();

    QVector<Command> requested;
    connect(&bar, &ActionBar::actionRequested, this,
            [&requested](Command cmd) { requested.append(cmd); });
    QVERIFY2(bar.resetButton()->isEnabled(), "precondition: 复位 must be usable");
    bar.resetButton()->click();

    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first(), Command::Reset);
    QVERIFY(!requested.contains(Command::HomeStart));
}

QTEST_MAIN(HomeStartButtonDeveloperTest)
#include "home_start_button_developer_test.moc"
