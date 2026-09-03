// Task 9 unit tests: IVisionService OpenCV adapter (spec §6, §7.4, §13).
//
// The vision module is isolated: the port interface (src/ports/ivision_service.h)
// is OpenCV-free, and the self-test runs on the adapter's own worker thread
// (spec §7.4) so it can never enter the PLC control path. A failed self-test
// only marks the vision diagnostic red; PLC control keeps working (spec §13).
//
// Coverage required by the task brief:
// - Self-test success: version reported, healthy.
// - Self-test failure (injected): failure reported, not healthy.
// - Self-test runs on the adapter's own worker thread, not the caller's.
// - Vision failure does not affect PLC control (estop still works).
// - Build-disabled (HLM_ENABLE_VISION=OFF): verified by a scratch CMake
//   configure, not by a unit test (see task report).

#include <QtTest>

#include <QSignalSpy>
#include <QThread>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/vision/vision_service.h"
#include "application/control_coordinator.h"

using namespace hlm;

class VisionAdapterTest : public QObject
{
    Q_OBJECT

private slots:
    void selfTestSuccessReportsVersion();
    void selfTestRunsOnWorkerThread();
    void forcedFailureReportsFailure();
    void visionFailureDoesNotBlockPlcControl();
};

void VisionAdapterTest::selfTestSuccessReportsVersion()
{
    VisionService service;
    QSignalSpy passed(&service, &IVisionService::selfTestPassed);
    QSignalSpy failed(&service, &IVisionService::selfTestFailed);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(passed.size() > 0, 5000);
    QCOMPARE(failed.size(), 0);
    QVERIFY(service.isHealthy());
    QVERIFY(!service.version().isEmpty());
    // The signal carries the same version string the port reports.
    QCOMPARE(passed.first().first().toString(), service.version());
    service.stop();
}

void VisionAdapterTest::selfTestRunsOnWorkerThread()
{
    VisionService service;
    QSignalSpy passed(&service, &IVisionService::selfTestPassed);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(passed.size() > 0, 5000);
    // Spec §7.4: the self-test runs on the adapter's own worker thread, never
    // on the caller's (UI/PLC) thread.
    QVERIFY(service.thread() != QThread::currentThread());
    service.stop();
}

void VisionAdapterTest::forcedFailureReportsFailure()
{
    VisionService service(/*forceSelfTestFailure=*/true);
    QSignalSpy passed(&service, &IVisionService::selfTestPassed);
    QSignalSpy failed(&service, &IVisionService::selfTestFailed);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(failed.size() > 0, 5000);
    QCOMPARE(passed.size(), 0);
    QVERIFY(!service.isHealthy());
    service.stop();
}

void VisionAdapterTest::visionFailureDoesNotBlockPlcControl()
{
    // Spec §13: OpenCV init failure -> vision diagnostic red, PLC control
    // continues to work. The vision adapter is fully independent of the PLC
    // control path; prove it by failing vision and still driving an estop
    // through the ControlCoordinator + SimulatedPlcGateway.
    VisionService vision(/*forceSelfTestFailure=*/true);
    QSignalSpy failed(&vision, &IVisionService::selfTestFailed);
    vision.start();
    QTRY_VERIFY_WITH_TIMEOUT(failed.size() > 0, 5000);
    QVERIFY(!vision.isHealthy());

    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) {
        gw.writeCoil(a, true);
        gw.writeCoil(a, false);
        return true;
    };
    t.writeHold = [&gw](quint16 a, bool v) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) {
        gw.writeRegister(a, v);
        return true;
    };
    ControlCoordinator c(t, {}, [&now]() { return now; });
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, &c,
                     [&c](const DeviceSnapshot &s) { c.onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, &c,
                     [&c](bool online) { c.onConnectionChanged(online); });
    QObject::connect(&gw, &SimulatedPlcGateway::writeCompleted, &c,
                     [&c](quint16 a, bool ok, const QString &) { c.onWriteCompleted(a, ok); });
    // Feed the snapshot/online state published before the coordinator existed.
    if (gw.hasSnapshot())
        c.onSnapshot(gw.lastSnapshot());
    c.onConnectionChanged(gw.isOnline());
    c.setRole(Role::Anonymous);

    QVERIFY(c.estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());
    vision.stop();
}

QTEST_GUILESS_MAIN(VisionAdapterTest)
#include "test_vision_adapter.moc"
