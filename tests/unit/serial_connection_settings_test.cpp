// PLC-HMI-004 black-box unit tests: transport-neutral serial settings and
// discovered-port descriptor domain values (brief OB-1, OB-2).
//
// Authored only from the sanitized behavior brief .ai/test-briefs/PLC-HMI-004.yaml
// and the approved .ai/project-contract.yaml. No production implementation
// source was read.
//
// Frozen API surface (contract-literal paths and field names):
//   src/domain/serial_connection_settings.h : hlm::SerialConnectionSettings
//     members port_name, station, baud_rate, stop_bits, parity, timeout_ms,
//     read_retries
//   src/domain/serial_port_descriptor.h : hlm::SerialPortDescriptor
//     members port_name, description, manufacturer, optional_vendor_id,
//     optional_product_id
//
// The contract fixes the *meaning* of the parity default (无 parity) and of the
// optional vendor/product ids but not their C++ representation. The helpers
// below are representation-agnostic: an enum/int/string/optional neutral
// encoding all satisfy the assertion, so the tests check documented semantics
// instead of an invented representation.
//
// Expected RED: compile failure against the current tree; the two domain
// headers do not exist yet.
//
// Coverage limitation recorded in the RED report: OB-1's "contains no Qt
// serial/widget/transport-handle/SQL members" and OB-2's "never exposes an open
// handle, probe result or claimed PLC identity" are absence properties; proving
// them requires reading production headers, which independence forbids. The
// module link set (hlm_core depends on Qt6::Core only) and module review are the
// enforcement mechanism for those properties.

#include <QtTest>

#include <QString>
#include <type_traits>

#include "domain/serial_connection_settings.h"
#include "domain/serial_port_descriptor.h"

using namespace hlm;

namespace {

// The documented neutral default of the `parity` field, representation-agnostic:
// a disengaged optional, the first enum value, integer 0, an empty string, or a
// "none"/"无" string all count as the neutral default.
template <typename T>
bool isNeutralParity(const T &value)
{
    if constexpr (requires { value.has_value(); }) {
        if (!value.has_value())
            return true;
        return isNeutralParity(*value);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<std::underlying_type_t<T>>(value) == 0;
    } else if constexpr (std::is_integral_v<T>) {
        return value == 0;
    } else if constexpr (std::is_same_v<std::decay_t<T>, QString>) {
        return value.isEmpty()
            || value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("无"), Qt::CaseInsensitive) == 0;
    } else {
        return value == T{};
    }
}

// Assign a parity that is observably not the neutral default.
template <typename T>
void setAlternateParity(T &value)
{
    if constexpr (requires {
                      typename T::value_type;
                      value.emplace(static_cast<typename T::value_type>(1));
                  }) {
        value.emplace(static_cast<typename T::value_type>(1));
    } else if constexpr (std::is_enum_v<T>) {
        value = static_cast<T>(static_cast<std::underlying_type_t<T>>(value) + 1);
    } else if constexpr (std::is_integral_v<T>) {
        value = (value == 0 ? 1 : 0);
    } else if constexpr (std::is_same_v<std::decay_t<T>, QString>) {
        value = QStringLiteral("even");
    } else {
        value = T{};
    }
}

// A disengaged optional id, representation-agnostic (optional, or a
// zero/empty sentinel).
template <typename T>
bool isDisengagedId(const T &value)
{
    if constexpr (requires { value.has_value(); })
        return !value.has_value();
    else
        return value == T{};
}

template <typename T>
void setVendorId(T &value)
{
    if constexpr (requires {
                      typename T::value_type;
                      value.emplace(static_cast<typename T::value_type>(0x1A86));
                  }) {
        value.emplace(static_cast<typename T::value_type>(0x1A86));
    } else if constexpr (std::is_integral_v<T>) {
        value = static_cast<T>(0x1A86);
    } else {
        (void)value;
    }
}

template <typename T>
qint64 idNumber(const T &value)
{
    if constexpr (requires { value.has_value(); })
        return value.has_value() ? qint64(*value) : qint64(-1);
    else if constexpr (std::is_arithmetic_v<T>)
        return qint64(value);
    else
        return qint64(-1);
}

} // namespace

class SerialConnectionSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1: neutral domain value with the documented defaults ------------
    void defaultsMatchTheDocumentedNeutralValues();
    void everyFieldRoundTripsThroughValueCopies();

    // --- OB-2: discovered-port descriptor identity fields -------------------
    void descriptorCarriesTheDocumentedIdentityFields();
    void descriptorDefaultsDoNotClaimAPortIdentity();
    void descriptorsAreIndependentValues();
};

// --- OB-1 ---------------------------------------------------------------------

void SerialConnectionSettingsTest::defaultsMatchTheDocumentedNeutralValues()
{
    const SerialConnectionSettings settings;

    QCOMPARE(settings.port_name, QStringLiteral("COM1"));
    QCOMPARE(int(settings.station), 1);
    QCOMPARE(int(settings.baud_rate), 9600);
    QCOMPARE(int(settings.stop_bits), 1);
    QVERIFY2(isNeutralParity(settings.parity),
             "the default parity must be the documented neutral (无 parity) value");
    QCOMPARE(int(settings.timeout_ms), 200);
    QCOMPARE(int(settings.read_retries), 1);

    // Defaults are per-value state: a second default-constructed value carries
    // the same neutral defaults and is not aliased to the first.
    const SerialConnectionSettings other;
    QCOMPARE(other.port_name, settings.port_name);
    QCOMPARE(int(other.station), int(settings.station));
    QCOMPARE(int(other.baud_rate), int(settings.baud_rate));
    QCOMPARE(int(other.stop_bits), int(settings.stop_bits));
    QVERIFY(other.parity == settings.parity);
    QCOMPARE(int(other.timeout_ms), int(settings.timeout_ms));
    QCOMPARE(int(other.read_retries), int(settings.read_retries));
}

void SerialConnectionSettingsTest::everyFieldRoundTripsThroughValueCopies()
{
    SerialConnectionSettings original;
    original.port_name = QStringLiteral("COM7");
    original.station = 2;
    original.baud_rate = 19200;
    original.stop_bits = 2;
    setAlternateParity(original.parity);
    original.timeout_ms = 500;
    original.read_retries = 3;

    QVERIFY2(!isNeutralParity(original.parity),
             "the alternate parity assignment did not produce a non-neutral parity");

    const SerialConnectionSettings copy = original;
    QCOMPARE(copy.port_name, QStringLiteral("COM7"));
    QCOMPARE(int(copy.station), 2);
    QCOMPARE(int(copy.baud_rate), 19200);
    QCOMPARE(int(copy.stop_bits), 2);
    QVERIFY(copy.parity == original.parity);
    QCOMPARE(int(copy.timeout_ms), 500);
    QCOMPARE(int(copy.read_retries), 3);

    // Value semantics: mutating a copy must not change the original value.
    SerialConnectionSettings mutated = copy;
    mutated.port_name = QStringLiteral("COM8");
    mutated.station = 9;
    QCOMPARE(original.port_name, QStringLiteral("COM7"));
    QCOMPARE(int(original.station), 2);
}

// --- OB-2 ---------------------------------------------------------------------

void SerialConnectionSettingsTest::descriptorCarriesTheDocumentedIdentityFields()
{
    SerialPortDescriptor descriptor;
    descriptor.port_name = QStringLiteral("/dev/ttyUSB0");
    descriptor.description = QStringLiteral("USB-SERIAL CH340");
    descriptor.manufacturer = QStringLiteral("wch.cn");
    setVendorId(descriptor.optional_vendor_id);
    setVendorId(descriptor.optional_product_id);

    QCOMPARE(descriptor.port_name, QStringLiteral("/dev/ttyUSB0"));
    QCOMPARE(descriptor.description, QStringLiteral("USB-SERIAL CH340"));
    QCOMPARE(descriptor.manufacturer, QStringLiteral("wch.cn"));
    QVERIFY2(!isDisengagedId(descriptor.optional_vendor_id),
             "an assigned vendor id must be carried by the descriptor");
    QCOMPARE(idNumber(descriptor.optional_vendor_id), qint64(0x1A86));
    QVERIFY2(!isDisengagedId(descriptor.optional_product_id),
             "an assigned product id must be carried by the descriptor");
    QCOMPARE(idNumber(descriptor.optional_product_id), qint64(0x1A86));
}

void SerialConnectionSettingsTest::descriptorDefaultsDoNotClaimAPortIdentity()
{
    const SerialPortDescriptor descriptor;
    QVERIFY2(descriptor.port_name.isEmpty(),
             "a default descriptor must not name a port");
    QVERIFY2(descriptor.description.isEmpty(),
             "a default descriptor must not claim a description");
    QVERIFY2(descriptor.manufacturer.isEmpty(),
             "a default descriptor must not claim a manufacturer");
    QVERIFY2(isDisengagedId(descriptor.optional_vendor_id),
             "a default descriptor must not claim a vendor id");
    QVERIFY2(isDisengagedId(descriptor.optional_product_id),
             "a default descriptor must not claim a product id");
}

void SerialConnectionSettingsTest::descriptorsAreIndependentValues()
{
    SerialPortDescriptor first;
    first.port_name = QStringLiteral("COM1");
    SerialPortDescriptor second;
    second.port_name = QStringLiteral("COM3");

    QVERIFY2(first.port_name != second.port_name,
             "two descriptors must be able to carry different port identities");

    second = first;
    QCOMPARE(second.port_name, first.port_name);
    second.port_name = QStringLiteral("COM4");
    QCOMPARE(first.port_name, QStringLiteral("COM1"));
}

QTEST_GUILESS_MAIN(SerialConnectionSettingsTest)
#include "serial_connection_settings_test.moc"
