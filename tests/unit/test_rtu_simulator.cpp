// Task 18 unit tests: standalone RTU simulator backend (spec §14.3, §15.5).
//
// The testable core (RtuRequestHandler) is driven directly: function codes
// 01/03/05/06, register mapping, and the PDU-level fault injections (timeout,
// exception response, illegal value, delay, heartbeat freeze, fault code).
// The Qt serial wiring (RtuServer) is exercised over a Linux pty so no
// virtual COM pair is required (acceptance: backend unit tests run without
// virtual COM). Station filtering is Qt's ADU-level behavior; the wire test
// proves that a wrong station is ignored while the configured station is
// answered, and the config test proves the server rejects 0/255/>247.

#include <QtTest>

#include <QDateTime>
#include <QElapsedTimer>
#include <QModbusExceptionResponse>
#include <QModbusRequest>
#include <QModbusResponse>
#include <QThread>
#include <QVector>

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulation_clock.h"
#include "tools/plc_simulator/fault_injection.h"
#include "tools/plc_simulator/rtu_server.h"

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

using namespace hlm;

namespace {

// --- request builders --------------------------------------------------------

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

// --- RTU framing helpers (wire tests) ---------------------------------------

quint16 crc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (char c : data) {
        crc ^= quint8(c);
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? quint16((crc >> 1) ^ 0xA001) : quint16(crc >> 1);
    }
    return crc;
}

QByteArray rtuFrame(quint8 station, quint8 functionCode, const QByteArray &pdu)
{
    QByteArray a;
    a.append(char(station));
    a.append(char(functionCode));
    a += pdu;
    const quint16 c = crc16(a);
    a.append(char(c & 0xFF));
    a.append(char(c >> 8));
    return a;
}

bool validRtuFrame(const QByteArray &frame)
{
    if (frame.size() < 4)
        return false;
    const QByteArray body = frame.left(frame.size() - 2);
    const quint16 crc = quint16(quint8(frame[frame.size() - 2]))
        | (quint16(quint8(frame[frame.size() - 1])) << 8);
    return crc16(body) == crc;
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

QByteArray readAvailable(int fd)
{
    QByteArray data;
    char buf[256];
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 0) <= 0)
            break;
        const int n = int(read(fd, buf, sizeof(buf)));
        if (n <= 0)
            break;
        data.append(buf, n);
    }
    return data;
}

#endif // Q_OS_UNIX

} // namespace

class RtuSimulatorTest : public QObject
{
    Q_OBJECT

private slots:
    // --- handler: function codes and register mapping ------------------------
    void readCoilsMapsToModelCoils();
    void readHoldingRegistersMapsToModelRegisters();
    void writeSingleCoilWritesModel();
    void writeSingleRegisterWritesModel();
    void writeSingleCoilRejectsInvalidValue();
    void outOfRangeAddressReturnsIllegalDataAddress();
    void unknownFunctionReturnsIllegalFunction();

    // --- handler: fault injection --------------------------------------------
    void timeoutInjectionReturnsNoResponse();
    void exceptionInjectionReturnsExceptionCode();
    void illegalValueInjectionRejectsWrites();
    void delayInjectionDelaysResponse();
    void heartbeatFreezeStopsD140();
    void faultCodeInjectionSetsModelFault();
    void requestLogRecordsRequests();

    // --- server: station configuration ---------------------------------------
    void stationValidationRejectsInvalidStations();

#if defined(Q_OS_UNIX)
    // --- server: wire tests over a pty (no virtual COM needed) --------------
    void wireStationFilteringIgnoresWrongStation();
    void wireDisconnectInjectionDropsLink();
#endif
};

// --- handler: function codes and register mapping -----------------------------

void RtuSimulatorTest::readCoilsMapsToModelCoils()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    model.writeCoil(50, true);
    model.writeCoil(52, true);

    const QModbusResponse resp = handler.handleRequest(readCoilsReq(50, 4));
    QVERIFY(resp.isValid());
    QCOMPARE(resp.functionCode(), QModbusRequest::ReadCoils);
    // PDU: byteCount(1) + one bit byte. M50 -> bit0, M51 -> bit1, M52 -> bit2.
    const QByteArray data = resp.data();
    QCOMPARE(data.size(), 2);
    QCOMPARE(quint8(data[0]), quint8(1));
    QCOMPARE(quint8(data[1]), quint8(0b00000101));
}

void RtuSimulatorTest::readHoldingRegistersMapsToModelRegisters()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    model.writeRegister(100, 0x1234);
    model.writeRegister(101, 0x5678);

    const QModbusResponse resp = handler.handleRequest(readHoldingRegistersReq(100, 2));
    QVERIFY(resp.isValid());
    QCOMPARE(resp.functionCode(), QModbusRequest::ReadHoldingRegisters);
    // PDU: byteCount(1) + 4 bytes big-endian.
    const QByteArray data = resp.data();
    QCOMPARE(data.size(), 5);
    QCOMPARE(quint8(data[0]), quint8(4));
    QCOMPARE(quint8(data[1]), quint8(0x12));
    QCOMPARE(quint8(data[2]), quint8(0x34));
    QCOMPARE(quint8(data[3]), quint8(0x56));
    QCOMPARE(quint8(data[4]), quint8(0x78));
}

void RtuSimulatorTest::writeSingleCoilWritesModel()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    const QModbusResponse on = handler.handleRequest(writeSingleCoilReq(50, true));
    QVERIFY(on.isValid());
    QVERIFY(model.readCoil(50));
    // Echo response: address + 0xFF00.
    const QByteArray onData = on.data();
    QCOMPARE(onData.size(), 4);
    QCOMPARE(quint8(onData[0]), quint8(0));
    QCOMPARE(quint8(onData[1]), quint8(50));
    QCOMPARE(quint8(onData[2]), quint8(0xFF));
    QCOMPARE(quint8(onData[3]), quint8(0x00));

    const QModbusResponse off = handler.handleRequest(writeSingleCoilReq(50, false));
    QVERIFY(off.isValid());
    QVERIFY(!model.readCoil(50));
}

void RtuSimulatorTest::writeSingleRegisterWritesModel()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    const QModbusResponse resp = handler.handleRequest(writeSingleRegisterReq(100, 0xABCD));
    QVERIFY(resp.isValid());
    QCOMPARE(model.readRegister(100), quint16(0xABCD));
    // Echo response: address + value.
    const QByteArray data = resp.data();
    QCOMPARE(data.size(), 4);
    QCOMPARE(quint8(data[0]), quint8(0));
    QCOMPARE(quint8(data[1]), quint8(100));
    QCOMPARE(quint8(data[2]), quint8(0xAB));
    QCOMPARE(quint8(data[3]), quint8(0xCD));
}

void RtuSimulatorTest::writeSingleCoilRejectsInvalidValue()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    const QModbusResponse resp = handler.handleRequest(
        QModbusRequest(QModbusRequest::WriteSingleCoil, quint16(50), quint16(0x1234)));
    QVERIFY(resp.isException());
    QCOMPARE(resp.exceptionCode(), QModbusExceptionResponse::IllegalDataValue);
    QVERIFY(!model.readCoil(50));
}

void RtuSimulatorTest::outOfRangeAddressReturnsIllegalDataAddress()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    // Coil space is M0-M112 (113 coils): address 113 is out of range.
    const QModbusResponse coil = handler.handleRequest(readCoilsReq(113, 1));
    QVERIFY(coil.isException());
    QCOMPARE(coil.exceptionCode(), QModbusExceptionResponse::IllegalDataAddress);

    // Register space is D100-D223 (224 registers): address 224 is out of range.
    const QModbusResponse reg = handler.handleRequest(readHoldingRegistersReq(224, 1));
    QVERIFY(reg.isException());
    QCOMPARE(reg.exceptionCode(), QModbusExceptionResponse::IllegalDataAddress);

    // A read that crosses the boundary is also rejected.
    const QModbusResponse cross = handler.handleRequest(readHoldingRegistersReq(223, 2));
    QVERIFY(cross.isException());
    QCOMPARE(cross.exceptionCode(), QModbusExceptionResponse::IllegalDataAddress);
}

void RtuSimulatorTest::unknownFunctionReturnsIllegalFunction()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    const QModbusResponse resp = handler.handleRequest(
        QModbusRequest(QModbusRequest::ReadDiscreteInputs, quint16(0), quint16(1)));
    QVERIFY(resp.isException());
    QCOMPARE(resp.exceptionCode(), QModbusExceptionResponse::IllegalFunction);
}

// --- handler: fault injection -------------------------------------------------

void RtuSimulatorTest::timeoutInjectionReturnsNoResponse()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    faults.setScenario(FaultInjector::Scenario::Timeout);
    const QModbusResponse resp = handler.handleRequest(readCoilsReq(0, 1));
    QVERIFY(!resp.isValid()); // invalid response -> the RTU layer sends nothing
}

void RtuSimulatorTest::exceptionInjectionReturnsExceptionCode()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    faults.setScenario(FaultInjector::Scenario::ExceptionResponse);
    faults.setExceptionCode(quint8(QModbusExceptionResponse::ServerDeviceFailure));
    const QModbusResponse resp = handler.handleRequest(readCoilsReq(0, 1));
    QVERIFY(resp.isException());
    QCOMPARE(resp.exceptionCode(), QModbusExceptionResponse::ServerDeviceFailure);
}

void RtuSimulatorTest::illegalValueInjectionRejectsWrites()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    faults.setScenario(FaultInjector::Scenario::IllegalValue);

    // Writes are answered with IllegalDataValue and do not touch the model.
    const QModbusResponse write = handler.handleRequest(writeSingleRegisterReq(100, 5));
    QVERIFY(write.isException());
    QCOMPARE(write.exceptionCode(), QModbusExceptionResponse::IllegalDataValue);
    QCOMPARE(model.readRegister(100), quint16(0));

    const QModbusResponse coil = handler.handleRequest(writeSingleCoilReq(50, true));
    QVERIFY(coil.isException());
    QCOMPARE(coil.exceptionCode(), QModbusExceptionResponse::IllegalDataValue);
    QVERIFY(!model.readCoil(50));

    // Reads still work normally.
    const QModbusResponse read = handler.handleRequest(readHoldingRegistersReq(100, 1));
    QVERIFY(read.isValid());
}

void RtuSimulatorTest::delayInjectionDelaysResponse()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    faults.setScenario(FaultInjector::Scenario::Delay);
    faults.setDelayMs(150);

    QElapsedTimer timer;
    timer.start();
    const QModbusResponse resp = handler.handleRequest(readCoilsReq(0, 1));
    const qint64 elapsed = timer.elapsed();

    QVERIFY(resp.isValid());
    QVERIFY2(elapsed >= 150, qPrintable(QStringLiteral("elapsed=%1ms").arg(elapsed)));
}

void RtuSimulatorTest::heartbeatFreezeStopsD140()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    const quint16 before = model.readRegister(140);
    handler.tick();
    QCOMPARE(model.readRegister(140), quint16(before + 1));

    faults.setScenario(FaultInjector::Scenario::HeartbeatFreeze);
    const quint16 frozen = model.readRegister(140);
    handler.tick();
    handler.tick();
    QCOMPARE(model.readRegister(140), frozen);
}

void RtuSimulatorTest::faultCodeInjectionSetsModelFault()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    faults.setScenario(FaultInjector::Scenario::FaultCode);
    faults.setFaultCode(8);

    // Reset + home return; the injected fault code surfaces on completion.
    model.writeCoil(103, true);
    model.writeCoil(103, false);
    QVERIFY(model.readCoil(50)); // homing
    handler.tick();
    handler.tick(); // home return completes after 2 s
    QVERIFY(!model.readCoil(50));
    QVERIFY(model.readCoil(14)); // latched fault
    QCOMPARE(model.readRegister(110), quint16(8));
}

void RtuSimulatorTest::requestLogRecordsRequests()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuRequestHandler handler(model, faults);

    handler.handleRequest(readCoilsReq(0, 1));
    handler.handleRequest(writeSingleRegisterReq(100, 42));
    faults.setScenario(FaultInjector::Scenario::ExceptionResponse);
    faults.setExceptionCode(quint8(QModbusExceptionResponse::ServerDeviceFailure));
    handler.handleRequest(readHoldingRegistersReq(100, 1));

    const auto log = handler.requestLog();
    QCOMPARE(log.size(), 3);

    QCOMPARE(log[0].functionCode, quint8(0x01));
    QCOMPARE(log[0].address, quint16(0));
    QCOMPARE(log[0].count, quint16(1));
    QVERIFY(log[0].responded);
    QCOMPARE(log[0].exceptionCode, quint8(0));

    QCOMPARE(log[1].functionCode, quint8(0x06));
    QCOMPARE(log[1].address, quint16(100));
    QCOMPARE(log[1].value, quint16(42));
    QVERIFY(log[1].responded);

    QCOMPARE(log[2].functionCode, quint8(0x03));
    QVERIFY(log[2].responded); // an exception response is still a response
    QCOMPARE(log[2].exceptionCode, quint8(QModbusExceptionResponse::ServerDeviceFailure));

    handler.clearRequestLog();
    QCOMPARE(handler.requestLog().size(), 0);
}

// --- server: station configuration -------------------------------------------

void RtuSimulatorTest::stationValidationRejectsInvalidStations()
{
    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);

    // Spec §14.3: an explicit station in 1-247; 0/255 would answer broadcast
    // frames and mask addressing errors.
    QVERIFY(!server.start(QStringLiteral("dummy"), 9600, 0));
    QVERIFY(!server.start(QStringLiteral("dummy"), 9600, 255));
    QVERIFY(!server.start(QStringLiteral("dummy"), 9600, 248));
    QVERIFY(!server.isRunning());
}

#if defined(Q_OS_UNIX)

// --- server: wire tests over a pty (no virtual COM needed) -------------------

void RtuSimulatorTest::wireStationFilteringIgnoresWrongStation()
{
    QString slave;
    const int master = openPty(&slave);
    QVERIFY2(master >= 0, "posix_openpt failed");

    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    QVERIFY(server.start(slave, 9600, 1));

    // Wrong station (2): the frame is ignored, no response.
    const QByteArray wrong = rtuFrame(2, 0x03, QByteArray::fromHex("00640001"));
    QVERIFY(write(master, wrong.constData(), wrong.size()) > 0);
    QTest::qWait(300);
    QCOMPARE(readAvailable(master).size(), 0);

    // Correct station (1): answered with the D100 value.
    const QByteArray right = rtuFrame(1, 0x03, QByteArray::fromHex("00640001"));
    QVERIFY(write(master, right.constData(), right.size()) > 0);
    QByteArray resp;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        resp += readAvailable(master); // accumulate: QTRY re-evaluates
        return !resp.isEmpty();
    }(), 2000);
    QVERIFY(validRtuFrame(resp));
    QCOMPARE(quint8(resp[0]), quint8(1)); // station
    QCOMPARE(quint8(resp[1]), quint8(0x03)); // function code
    QCOMPARE(quint8(resp[2]), quint8(2)); // byte count
    const quint16 d100 = model.readRegister(100);
    QCOMPARE(quint8(resp[3]), quint8(d100 >> 8));
    QCOMPARE(quint8(resp[4]), quint8(d100 & 0xFF));

    server.stop();
    close(master);
}

void RtuSimulatorTest::wireDisconnectInjectionDropsLink()
{
    QString slave;
    const int master = openPty(&slave);
    QVERIFY2(master >= 0, "posix_openpt failed");

    SimulationClock clock;
    H3uSimulationModel model(clock);
    FaultInjector faults;
    RtuServer server(model, faults);
    QVERIFY(server.start(slave, 9600, 1));

    const QByteArray req = rtuFrame(1, 0x03, QByteArray::fromHex("00640001"));

    // Link works first.
    QVERIFY(write(master, req.constData(), req.size()) > 0);
    QByteArray resp;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        resp += readAvailable(master); // accumulate: QTRY re-evaluates
        return !resp.isEmpty();
    }(), 2000);
    QVERIFY(validRtuFrame(resp));

    // Disconnect injection: the request is consumed, the link drops.
    faults.setScenario(FaultInjector::Scenario::Disconnect);
    QVERIFY(write(master, req.constData(), req.size()) > 0);
    QTest::qWait(300);
    QCOMPARE(readAvailable(master).size(), 0);
    QVERIFY(!server.isRunning());

    // Clear the scenario and restart: the link works again.
    faults.setScenario(FaultInjector::Scenario::None);
    QVERIFY(server.start(slave, 9600, 1));
    QVERIFY(write(master, req.constData(), req.size()) > 0);
    resp.clear();
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        resp += readAvailable(master); // accumulate: QTRY re-evaluates
        return !resp.isEmpty();
    }(), 2000);
    QVERIFY(validRtuFrame(resp));

    server.stop();
    close(master);
}

#endif // Q_OS_UNIX

QTEST_MAIN(RtuSimulatorTest)
#include "test_rtu_simulator.moc"
