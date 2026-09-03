// Task 19 unit tests: RTU simulator control panel (spec §14.3).
//
// Coverage required by the task brief:
// - 场景选择只调用后端接口: selecting a scenario changes FaultInjector state
//   and never touches the model directly.
// - 状态回显: connection status and register display are correct.
// - 无效配置: station out of range / empty COM port / invalid baud rejected.
// - 触摸/鼠标操作: buttons clickable, 48px touch targets (spec §11.1).
// - 请求日志可读: injected requests produce readable log entries, clearable.
//
// The panel model (ControlPanelModel) is driven directly for the logic tests;
// the widget (ControlPanel) is exercised for the touch/mouse and rendering
// tests. Widget tests run headless via QT_QPA_PLATFORM=offscreen.

#include <QtTest>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulation_clock.h"
#include "tools/plc_simulator/control_panel.h"
#include "tools/plc_simulator/fault_injection.h"
#include "tools/plc_simulator/rtu_server.h"

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace hlm;

namespace {

QModbusRequest readCoilsReq(quint16 addr, quint16 count)
{
    return QModbusRequest(QModbusRequest::ReadCoils, addr, count);
}

QModbusRequest readHoldingRegistersReq(quint16 addr, quint16 count)
{
    return QModbusRequest(QModbusRequest::ReadHoldingRegisters, addr, count);
}

QModbusRequest writeSingleCoilReq(quint16 addr, bool on)
{
    return QModbusRequest(QModbusRequest::WriteSingleCoil, addr,
                          on ? quint16(0xFF00) : quint16(0x0000));
}

QModbusRequest writeSingleRegisterReq(quint16 addr, quint16 value)
{
    return QModbusRequest(QModbusRequest::WriteSingleRegister, addr, value);
}

// Sends a synthetic click (press + release) at the widget center.
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

#if defined(Q_OS_UNIX)

int openPty(QString *slaveName)
{
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    *slaveName = QString::fromLatin1(ptsname(master));
    return master;
}

#endif // Q_OS_UNIX

} // namespace

class ControlPanelTest : public QObject
{
    Q_OBJECT

private slots:
    // --- scenario selection only touches the backend -------------------------
    void scenarioSelectionOnlyTouchesFaultInjector();
    void conditionalFailureScenarioForcesM45();
    void scenarioSpinboxValuesReachFaultInjector();

    // --- state echo ----------------------------------------------------------
    void connectionStatusReflectsStartStop();
    void registerDisplayReflectsModel();

    // --- invalid config ------------------------------------------------------
    void invalidConfigRejected();

    // --- touch / mouse -------------------------------------------------------
    void buttonsClickable();
    void touchTargetsAtLeast48px();

    // --- request log ----------------------------------------------------------
    void requestLogReadableAndClearable();

    // --- simulation time -----------------------------------------------------
    void tickAdvancesSimulation();
};

// --- scenario selection only touches the backend ------------------------------

void ControlPanelTest::scenarioSelectionOnlyTouchesFaultInjector()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    // Snapshot the model state: selecting a scenario must not change it.
    const auto regsBefore = QVector<quint16>{
        model.readRegister(100), model.readRegister(110), model.readRegister(128),
        model.readRegister(130), model.readRegister(140),
    };
    const auto coilsBefore = QVector<bool>{
        model.readCoil(0), model.readCoil(14), model.readCoil(34),
        model.readCoil(44), model.readCoil(45), model.readCoil(50),
        model.readCoil(61),
    };

    const struct {
        FaultInjector::Scenario scenario;
        int exceptionCode;
        int delayMs;
        int faultCode;
    } cases[] = {
        {FaultInjector::Scenario::None, 0, 0, 0},
        {FaultInjector::Scenario::ConditionalFailure, 0, 0, 0},
        {FaultInjector::Scenario::Timeout, 0, 0, 0},
        {FaultInjector::Scenario::FaultCode, 0, 0, 8},
        {FaultInjector::Scenario::Disconnect, 0, 0, 0},
        {FaultInjector::Scenario::Delay, 0, 250, 0},
        {FaultInjector::Scenario::ExceptionResponse, 0x03, 0, 0},
        {FaultInjector::Scenario::IllegalValue, 0, 0, 0},
        {FaultInjector::Scenario::HeartbeatFreeze, 0, 0, 0},
    };

    for (const auto &c : cases) {
        panel.setScenario(c.scenario);
        panel.setExceptionCode(quint8(c.exceptionCode));
        panel.setDelayMs(c.delayMs);
        panel.setFaultCode(c.faultCode);

        // FaultInjector state changed correctly.
        QCOMPARE(faults.scenario(), c.scenario);
        QCOMPARE(int(faults.exceptionCode()), c.exceptionCode);
        QCOMPARE(faults.delayMs(), c.delayMs);
        QCOMPARE(faults.faultCode(), c.faultCode);

        // The model was not touched by the selection.
        QCOMPARE(model.readRegister(100), regsBefore[0]);
        QCOMPARE(model.readRegister(110), regsBefore[1]);
        QCOMPARE(model.readRegister(128), regsBefore[2]);
        QCOMPARE(model.readRegister(130), regsBefore[3]);
        QCOMPARE(model.readRegister(140), regsBefore[4]);
        QCOMPARE(model.readCoil(0), coilsBefore[0]);
        QCOMPARE(model.readCoil(14), coilsBefore[1]);
        QCOMPARE(model.readCoil(34), coilsBefore[2]);
        QCOMPARE(model.readCoil(44), coilsBefore[3]);
        QCOMPARE(model.readCoil(45), coilsBefore[4]);
        QCOMPARE(model.readCoil(50), coilsBefore[5]);
        QCOMPARE(model.readCoil(61), coilsBefore[6]);
    }
}

void ControlPanelTest::conditionalFailureScenarioForcesM45()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    panel.setScenario(FaultInjector::Scenario::ConditionalFailure);
    QCOMPARE(faults.scenario(), FaultInjector::Scenario::ConditionalFailure);

    // A width-adjust command is accepted (echo response) but fails its
    // preconditions: M45 set, positioning never starts (M34 stays clear).
    const QModbusResponse resp =
        server.handler().handleRequest(writeSingleCoilReq(43, true));
    QVERIFY(resp.isValid());
    QVERIFY(!resp.isException());
    QVERIFY(model.readCoil(45));
    QVERIFY(!model.readCoil(34));
    QVERIFY(!model.readCoil(44));
}

void ControlPanelTest::scenarioSpinboxValuesReachFaultInjector()
{
    // Task 19 review: the parameter spinboxes were dead UI — their values were
    // never forwarded to the FaultInjector. Pre-fill each spinbox, then select
    // the matching scenario and verify the value reached the backend.
    SimulationClock clock;
    H3uSimulationModel model(clock);
    ControlPanel widget(model);
    widget.show();
    QApplication::processEvents();

    // Delay: pre-fill 500 ms, then select 延迟.
    widget.delaySpin()->setValue(500);
    const int delayIndex =
        widget.scenarioCombo()->findText(QStringLiteral("延迟"));
    QVERIFY(delayIndex >= 0);
    widget.scenarioCombo()->setCurrentIndex(delayIndex);
    QCOMPARE(widget.faults().scenario(), FaultInjector::Scenario::Delay);
    QCOMPARE(widget.faults().delayMs(), 500);

    // Exception code: pre-fill 0x0B, then select 异常响应.
    widget.exceptionSpin()->setValue(0x0B);
    const int excIndex =
        widget.scenarioCombo()->findText(QStringLiteral("异常响应"));
    QVERIFY(excIndex >= 0);
    widget.scenarioCombo()->setCurrentIndex(excIndex);
    QCOMPARE(widget.faults().scenario(),
             FaultInjector::Scenario::ExceptionResponse);
    QCOMPARE(int(widget.faults().exceptionCode()), 0x0B);

    // Fault code: pre-fill 3, then select 故障代码.
    widget.faultCodeSpin()->setValue(3);
    const int fcIndex =
        widget.scenarioCombo()->findText(QStringLiteral("故障代码"));
    QVERIFY(fcIndex >= 0);
    widget.scenarioCombo()->setCurrentIndex(fcIndex);
    QCOMPARE(widget.faults().scenario(), FaultInjector::Scenario::FaultCode);
    QCOMPARE(widget.faults().faultCode(), 3);
}

// --- state echo ----------------------------------------------------------------

void ControlPanelTest::connectionStatusReflectsStartStop()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    // Not started yet.
    QVERIFY(!panel.isRunning());
    QVERIFY(panel.statusText().contains(QStringLiteral("未连接")));

    // Invalid port: start rejected, status shows the failure.
    panel.setPortName(QString());
    panel.setBaudRate(9600);
    panel.setStation(1);
    QVERIFY(!panel.start());
    QVERIFY(!panel.isRunning());
    QVERIFY(panel.statusText().contains(QStringLiteral("启动失败")));

#if defined(Q_OS_UNIX)
    // Valid pty port: start succeeds, status shows the connection.
    QString slave;
    const int master = openPty(&slave);
    QVERIFY2(master >= 0, "posix_openpt failed");
    panel.setPortName(slave);
    QVERIFY(panel.start());
    QVERIFY(panel.isRunning());
    QVERIFY(panel.statusText().contains(QStringLiteral("已连接")));
    QVERIFY(panel.statusText().contains(QStringLiteral("1"))); // station

    // Stop: status returns to 未连接.
    panel.stop();
    QVERIFY(!panel.isRunning());
    QVERIFY(panel.statusText().contains(QStringLiteral("未连接")));
    close(master);
#endif
}

void ControlPanelTest::registerDisplayReflectsModel()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    model.writeRegister(100, 0x1234);
    model.writeRegister(110, 8);
    model.writeRegister(140, 42);
    model.writeCoil(0, true);
    model.writeCoil(14, true);
    model.writeCoil(45, true);

    QCOMPARE(panel.registerValue(100), quint16(0x1234));
    QCOMPARE(panel.registerValue(110), quint16(8));
    QCOMPARE(panel.registerValue(140), quint16(42));
    QVERIFY(panel.coilValue(0));
    QVERIFY(panel.coilValue(14));
    QVERIFY(panel.coilValue(45));

    // The widget renders the same state after refresh().
    ControlPanel widget(model);
    widget.refresh();
    QCOMPARE(widget.registerLabel(QStringLiteral("D100"))->text(),
             QStringLiteral("4660"));
    QCOMPARE(widget.registerLabel(QStringLiteral("D110"))->text(),
             QStringLiteral("8"));
    QCOMPARE(widget.registerLabel(QStringLiteral("D140"))->text(),
             QStringLiteral("42"));
    QCOMPARE(widget.registerLabel(QStringLiteral("M0"))->text(),
             QStringLiteral("1"));
    QCOMPARE(widget.registerLabel(QStringLiteral("M14"))->text(),
             QStringLiteral("1"));
    QCOMPARE(widget.registerLabel(QStringLiteral("M45"))->text(),
             QStringLiteral("1"));
    QCOMPARE(widget.registerLabel(QStringLiteral("M34"))->text(),
             QStringLiteral("0"));
}

// --- invalid config -------------------------------------------------------------

void ControlPanelTest::invalidConfigRejected()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    // Station out of range (spec §14.3: explicit station in 1-247).
    panel.setPortName(QStringLiteral("COM1"));
    panel.setBaudRate(9600);
    panel.setStation(0);
    QVERIFY(!panel.start());
    QVERIFY(panel.statusText().contains(QStringLiteral("站号")));

    panel.setStation(248);
    QVERIFY(!panel.start());
    QVERIFY(panel.statusText().contains(QStringLiteral("站号")));

    panel.setStation(255);
    QVERIFY(!panel.start());
    QVERIFY(panel.statusText().contains(QStringLiteral("站号")));

    // Empty COM port.
    panel.setStation(1);
    panel.setPortName(QString());
    QVERIFY(!panel.start());
    QVERIFY(panel.statusText().contains(QStringLiteral("串口")));

    // Invalid baud rate (only 9600/19200 are offered).
    panel.setPortName(QStringLiteral("COM1"));
    panel.setBaudRate(4800);
    QVERIFY(!panel.start());
    QVERIFY(panel.statusText().contains(QStringLiteral("波特率")));

    panel.setBaudRate(38400);
    QVERIFY(!panel.start());
    QVERIFY(panel.statusText().contains(QStringLiteral("波特率")));

    // Valid config but the port does not exist: rejected by the server.
    panel.setBaudRate(9600);
    QVERIFY(!panel.start());
    QVERIFY(!panel.isRunning());
}

// --- touch / mouse ---------------------------------------------------------------

void ControlPanelTest::buttonsClickable()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    ControlPanel widget(model);
    widget.show();
    QApplication::processEvents();

    // Start with an invalid config: the click is handled and the status
    // label shows the failure (no crash, no connection).
    widget.portEdit()->setText(QString());
    clickAt(widget.startButton());
    QApplication::processEvents();
    QVERIFY(!widget.server().isRunning());
    QVERIFY(widget.statusLabel()->text().contains(QStringLiteral("启动失败")));

    // Stop click is a no-op when not running.
    clickAt(widget.stopButton());
    QApplication::processEvents();
    QVERIFY(!widget.server().isRunning());

    // Scenario combo: selecting 超时 drives the FaultInjector.
    const int timeoutIndex = widget.scenarioCombo()->findText(
        QStringLiteral("超时"));
    QVERIFY(timeoutIndex >= 0);
    widget.scenarioCombo()->setCurrentIndex(timeoutIndex);
    QCOMPARE(widget.faults().scenario(), FaultInjector::Scenario::Timeout);

    // Clear-log click empties the log table.
    widget.server().handler().handleRequest(readCoilsReq(0, 1));
    widget.refresh();
    QVERIFY(widget.logTable()->rowCount() >= 1);
    clickAt(widget.clearLogButton());
    QApplication::processEvents();
    QCOMPARE(widget.logTable()->rowCount(), 0);
    QCOMPARE(widget.server().handler().requestLog().size(), 0);
}

void ControlPanelTest::touchTargetsAtLeast48px()
{
    // Touch targets >= 48 px (spec §11.1). Verify the rendered size, not just
    // the enforced minimum (Task 19 review: minimumHeight() >= 48 was a
    // tautology).
    SimulationClock clock;
    H3uSimulationModel model(clock);
    ControlPanel widget(model);
    widget.show();
    QApplication::processEvents();

    for (QPushButton *button : {widget.startButton(), widget.stopButton(),
                                widget.clearLogButton()}) {
        QVERIFY2(button->size().height() >= 48,
                 qPrintable(QStringLiteral("%1 rendered %2 px")
                                .arg(button->text())
                                .arg(button->size().height())));
    }
}

// --- request log -----------------------------------------------------------------

void ControlPanelTest::requestLogReadableAndClearable()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    server.handler().handleRequest(readCoilsReq(0, 1));
    server.handler().handleRequest(writeSingleRegisterReq(100, 42));
    server.handler().handleRequest(writeSingleCoilReq(43, true));
    faults.setScenario(FaultInjector::Scenario::ExceptionResponse);
    faults.setExceptionCode(quint8(QModbusExceptionResponse::ServerDeviceFailure));
    server.handler().handleRequest(readHoldingRegistersReq(100, 1));
    faults.setScenario(FaultInjector::Scenario::None);

    const QStringList lines = panel.requestLogLines();
    QCOMPARE(lines.size(), 4);

    // Readable format: function code, address, count/value, response, exception.
    QVERIFY2(lines[0].contains(QStringLiteral("读线圈")),
             qPrintable(lines[0]));
    QVERIFY2(lines[0].contains(QStringLiteral("M0")), qPrintable(lines[0]));
    QVERIFY2(lines[0].contains(QStringLiteral("已响应")), qPrintable(lines[0]));

    QVERIFY2(lines[1].contains(QStringLiteral("写寄存器")),
             qPrintable(lines[1]));
    QVERIFY2(lines[1].contains(QStringLiteral("D100")), qPrintable(lines[1]));
    QVERIFY2(lines[1].contains(QStringLiteral("42")), qPrintable(lines[1]));

    // Task 19 review: 0x05 write-coil must use the M prefix, not D.
    QVERIFY2(lines[2].contains(QStringLiteral("写线圈")),
             qPrintable(lines[2]));
    QVERIFY2(lines[2].contains(QStringLiteral("M43")), qPrintable(lines[2]));

    QVERIFY2(lines[3].contains(QStringLiteral("读寄存器")),
             qPrintable(lines[3]));
    QVERIFY2(lines[3].contains(QStringLiteral("异常")), qPrintable(lines[3]));
    QVERIFY2(lines[3].contains(QStringLiteral("0x04")), qPrintable(lines[3]));

    // The widget renders the same entries in its table.
    ControlPanel widget(model);
    widget.server().handler().handleRequest(readCoilsReq(0, 1));
    widget.server().handler().handleRequest(writeSingleRegisterReq(100, 42));
    widget.server().handler().handleRequest(writeSingleCoilReq(43, true));
    widget.faults().setScenario(FaultInjector::Scenario::ExceptionResponse);
    widget.faults().setExceptionCode(
        quint8(QModbusExceptionResponse::ServerDeviceFailure));
    widget.server().handler().handleRequest(readHoldingRegistersReq(100, 1));
    widget.refresh();
    QCOMPARE(widget.logTable()->rowCount(), 4);
    QCOMPARE(widget.logTable()->item(0, 0)->text(), QStringLiteral("01"));
    QCOMPARE(widget.logTable()->item(1, 1)->text(), QStringLiteral("D100"));
    QCOMPARE(widget.logTable()->item(2, 1)->text(), QStringLiteral("M43"));
    QCOMPARE(widget.logTable()->item(3, 5)->text(), QStringLiteral("0x04"));

    // Clearable.
    panel.clearRequestLog();
    QCOMPARE(panel.requestLogLines().size(), 0);
    QCOMPARE(server.handler().requestLog().size(), 0);
}

// --- simulation time ---------------------------------------------------------------

void ControlPanelTest::tickAdvancesSimulation()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    ControlPanelModel panel(server, faults, model);

    // The panel QTimer drives handler().tick(); the model exposes the same
    // step so tests advance simulated time deterministically.
    const quint16 before = model.readRegister(140);
    panel.tick();
    QCOMPARE(model.readRegister(140), quint16(before + 1));

    // Heartbeat freeze: tick() no longer advances D140 (dead PLC).
    panel.setScenario(FaultInjector::Scenario::HeartbeatFreeze);
    const quint16 frozen = model.readRegister(140);
    panel.tick();
    panel.tick();
    QCOMPARE(model.readRegister(140), frozen);
}

QTEST_MAIN(ControlPanelTest)
#include "test_control_panel.moc"
