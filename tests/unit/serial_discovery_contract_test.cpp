// PLC-HMI-004 black-box unit tests: the passive, asynchronous serial-port
// discovery boundary (brief OB-3; the interface half of OB-4).
//
// Authored only from the sanitized behavior brief .ai/test-briefs/PLC-HMI-004.yaml
// and the approved .ai/project-contract.yaml. No production implementation
// source was read.
//
// Frozen contract surface:
//   src/ports/iserial_port_discovery.h
//     class hlm::ISerialPortDiscovery   (abstract asynchronous boundary)
//       operation: enumerateAvailablePorts() returning the
//                  enumeration_request_id
//       completion carries exactly the required fields
//                  enumeration_request_id, discovered_descriptors,
//                  completion_error
//   src/domain/serial_port_descriptor.h : hlm::SerialPortDescriptor
//
// The contract names the required fields but not the C++ carrier of the
// asynchronous completion. The tests freeze the carrier as
// `hlm::SerialEnumerationResult` with those three contract-literal members,
// announced by an `enumerationCompleted(const SerialEnumerationResult &)`
// signal. This author assumption is recorded in the author-phase RED report so
// the implementation can align with the frozen baseline.
//
// Coverage note: "does not open, probe, sweep or identify any port" and the
// gateway passivity of the real adapter are not asserted here; the fake below
// has no probing capability at all and the passive composition-root behavior is
// asserted in tests/integration/serial_settings_application_test.cpp.
//
// Expected RED: compile failure against the current tree; the interface header
// does not exist yet.

#include <QtTest>

#include <QVector>
#include <type_traits>

#include "domain/serial_port_descriptor.h"
#include "ports/iserial_port_discovery.h"

using namespace hlm;

namespace {

SerialPortDescriptor descriptorFor(const QString &portName)
{
    SerialPortDescriptor descriptor;
    descriptor.port_name = portName;
    return descriptor;
}

// Scripted implementation of the boundary. It never touches any transport: it
// only answers requests with request ids and emits the scripted completion, so
// the tests observe the correlation contract itself.
class ScriptedSerialPortDiscovery : public ISerialPortDiscovery
{
    Q_OBJECT

public:
    quint64 enumerateAvailablePorts() override
    {
        ++requests;
        last_request_id = next_id;
        return next_id++;
    }

    void completeWith(quint64 requestId,
                      const QVector<SerialPortDescriptor> &ports,
                      const QString &error = QString())
    {
        SerialEnumerationResult result;
        result.enumeration_request_id = requestId;
        result.discovered_descriptors = ports;
        result.completion_error = error;
        emit enumerationCompleted(result);
    }

    int requests = 0;
    quint64 last_request_id = 0;

private:
    quint64 next_id = 1;
};

// Stand-in consumer for the OB-3 correlation requirement: it keeps only the
// outstanding request id and accepts a completion only when the carried
// enumeration_request_id matches it. This exercises the required completion
// fields; the real consumer (Application/page) is exercised in the integration
// target.
struct EnumerationRecorder
{
    void expect(quint64 requestId) { outstanding = requestId; }

    void onCompletion(const SerialEnumerationResult &result)
    {
        last_seen_id = result.enumeration_request_id;
        if (result.enumeration_request_id != outstanding)
            return;
        ++matches;
        last_descriptors = result.discovered_descriptors;
        last_error = result.completion_error;
    }

    quint64 outstanding = 0;
    quint64 last_seen_id = 0;
    int matches = 0;
    QVector<SerialPortDescriptor> last_descriptors;
    QString last_error;
};

} // namespace

class SerialDiscoveryContractTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-3: asynchronous, correlated enumeration -------------------------
    void interfaceIsAnAbstractPassiveBoundary();
    void requestsReturnNonZeroIdsAndCompletionsCarryTheSameId();
    void successiveRequestsReceiveDistinctIds();
    void zeroOneAndMultiplePortsAreAllValidCompletions();
    void failedEnumerationCarriesAnErrorWithoutClaimingPorts();
    void completionForAnUnmatchedIdCannotCompleteTheOutstandingRequest();
};

// --- OB-3 ---------------------------------------------------------------------

void SerialDiscoveryContractTest::interfaceIsAnAbstractPassiveBoundary()
{
    // The discovery boundary is a port interface, not a concrete transport.
    static_assert(std::is_abstract_v<ISerialPortDiscovery>,
                  "ISerialPortDiscovery must be an abstract port boundary");

    ScriptedSerialPortDiscovery discovery;
    QCOMPARE(discovery.requests, 0);
}

void SerialDiscoveryContractTest::requestsReturnNonZeroIdsAndCompletionsCarryTheSameId()
{
    ScriptedSerialPortDiscovery discovery;
    QVector<SerialEnumerationResult> completions;
    connect(&discovery, &ISerialPortDiscovery::enumerationCompleted, this,
            [&completions](const SerialEnumerationResult &result) {
                completions.append(result);
            });

    const quint64 requestId = discovery.enumerateAvailablePorts();
    QVERIFY2(requestId != 0,
             "an enumeration request must return a non-zero enumeration_request_id");
    QCOMPARE(discovery.last_request_id, requestId);

    discovery.completeWith(requestId,
                           {descriptorFor(QStringLiteral("COM3")),
                            descriptorFor(QStringLiteral("COM5"))});

    QCOMPARE(int(completions.size()), 1);
    QCOMPARE(completions[0].enumeration_request_id, requestId);
    QCOMPARE(int(completions[0].discovered_descriptors.size()), 2);
    QCOMPARE(completions[0].discovered_descriptors[0].port_name,
             QStringLiteral("COM3"));
    QCOMPARE(completions[0].discovered_descriptors[1].port_name,
             QStringLiteral("COM5"));
    QVERIFY2(completions[0].completion_error.isEmpty(),
             "a successful enumeration completion must not carry an error");
}

void SerialDiscoveryContractTest::successiveRequestsReceiveDistinctIds()
{
    ScriptedSerialPortDiscovery discovery;

    const quint64 first = discovery.enumerateAvailablePorts();
    const quint64 second = discovery.enumerateAvailablePorts();

    QVERIFY2(first != 0, "an enumeration request id must be non-zero");
    QVERIFY2(second != 0, "an enumeration request id must be non-zero");
    QVERIFY2(first != second,
             "successive enumeration requests must receive distinct request ids");
    QCOMPARE(discovery.requests, 2);
}

void SerialDiscoveryContractTest::zeroOneAndMultiplePortsAreAllValidCompletions()
{
    ScriptedSerialPortDiscovery discovery;
    QVector<SerialEnumerationResult> completions;
    connect(&discovery, &ISerialPortDiscovery::enumerationCompleted, this,
            [&completions](const SerialEnumerationResult &result) {
                completions.append(result);
            });

    const quint64 zero = discovery.enumerateAvailablePorts();
    discovery.completeWith(zero, {});

    const quint64 one = discovery.enumerateAvailablePorts();
    discovery.completeWith(one, {descriptorFor(QStringLiteral("COM3"))});

    const quint64 many = discovery.enumerateAvailablePorts();
    discovery.completeWith(many,
                           {descriptorFor(QStringLiteral("COM1")),
                            descriptorFor(QStringLiteral("COM3")),
                            descriptorFor(QStringLiteral("/dev/ttyUSB0"))});

    QCOMPARE(int(completions.size()), 3);

    QCOMPARE(int(completions[0].discovered_descriptors.size()), 0);
    QVERIFY2(completions[0].completion_error.isEmpty(),
             "an empty port list is a valid result, not an error");

    QCOMPARE(int(completions[1].discovered_descriptors.size()), 1);
    QCOMPARE(completions[1].discovered_descriptors[0].port_name,
             QStringLiteral("COM3"));

    QCOMPARE(int(completions[2].discovered_descriptors.size()), 3);
    QCOMPARE(completions[2].discovered_descriptors[2].port_name,
             QStringLiteral("/dev/ttyUSB0"));
    QVERIFY(completions[2].completion_error.isEmpty());
}

void SerialDiscoveryContractTest::failedEnumerationCarriesAnErrorWithoutClaimingPorts()
{
    ScriptedSerialPortDiscovery discovery;
    QVector<SerialEnumerationResult> completions;
    connect(&discovery, &ISerialPortDiscovery::enumerationCompleted, this,
            [&completions](const SerialEnumerationResult &result) {
                completions.append(result);
            });

    const quint64 requestId = discovery.enumerateAvailablePorts();
    discovery.completeWith(requestId, {}, QStringLiteral("枚举串口失败"));

    QCOMPARE(int(completions.size()), 1);
    QCOMPARE(completions[0].enumeration_request_id, requestId);
    QVERIFY2(!completions[0].completion_error.isEmpty(),
             "a failed enumeration completion must carry the error");
    QVERIFY2(completions[0].discovered_descriptors.isEmpty(),
             "a failed enumeration must not claim discovered ports");
}

void SerialDiscoveryContractTest::completionForAnUnmatchedIdCannotCompleteTheOutstandingRequest()
{
    ScriptedSerialPortDiscovery discovery;
    EnumerationRecorder recorder;
    connect(&discovery, &ISerialPortDiscovery::enumerationCompleted, this,
            [&recorder](const SerialEnumerationResult &result) {
                recorder.onCompletion(result);
            });

    const quint64 outstanding = discovery.enumerateAvailablePorts();
    recorder.expect(outstanding);

    // A completion for an unrelated/unknown request id arrives first.
    discovery.completeWith(outstanding + 1000, {descriptorFor(QStringLiteral("COM9"))});

    QCOMPARE(recorder.matches, 0);
    QCOMPARE(recorder.last_seen_id, outstanding + 1000);
    QVERIFY2(recorder.last_descriptors.isEmpty(),
             "an unmatched completion must not be accepted as the outstanding result");

    // The completion carrying the outstanding id is the one that completes it.
    discovery.completeWith(outstanding, {descriptorFor(QStringLiteral("COM1"))});

    QCOMPARE(recorder.matches, 1);
    QCOMPARE(int(recorder.last_descriptors.size()), 1);
    QCOMPARE(recorder.last_descriptors[0].port_name, QStringLiteral("COM1"));
    QVERIFY(recorder.last_error.isEmpty());
}

QTEST_GUILESS_MAIN(SerialDiscoveryContractTest)
#include "serial_discovery_contract_test.moc"
