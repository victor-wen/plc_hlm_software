// PLC-HMI-004 black-box unit tests: the administrator settings page presents
// discovered serial ports and isolated save state (brief OB-9; the duplicate
// half of OB-6). The page emits requests only; it is exercised with a
// ShellModel and without any database or serial transport.
//
// Authored only from the sanitized behavior brief .ai/test-briefs/PLC-HMI-004.yaml
// and the approved .ai/project-contract.yaml. No production implementation
// source was read.
//
// Frozen page surface (author assumptions recorded in the author-phase RED
// report; names follow the existing page conventions visible in inspectable
// tests):
//   UsersSettingsPage(ShellModel&)                       (existing)
//   serialPortComboBox()            -> QComboBox*
//   serialPortEdit()                -> QLineEdit*        (manual entry)
//   saveSerialSettingsButton()      -> QPushButton*
//   refreshSerialPortsButton()      -> QPushButton*      (explicit enumeration action)
//   serialSettingsStatusText()      -> QString           (page-local status)
//   serialSettings()                -> SerialConnectionSettings
//   setSerialSettings(const SerialConnectionSettings&)
//   setDiscoveredSerialPorts(const QVector<SerialPortDescriptor>&)
//   setSerialSettingsSavePending()
//   setSerialSettingsSaveResult(bool committed, const QString& error)
//   signals: saveSerialSettingsRequested(const SerialConnectionSettings&),
//            enumerateSerialPortsRequested()
//
// Expected RED: compile failure against the current tree; the serial page
// surface above does not exist yet.

#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QVector>

#include "domain/serial_connection_settings.h"
#include "domain/serial_port_descriptor.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

SerialPortDescriptor descriptorFor(const QString &portName,
                                   const QString &description = QString(),
                                   const QString &manufacturer = QString())
{
    SerialPortDescriptor descriptor;
    descriptor.port_name = portName;
    descriptor.description = description;
    descriptor.manufacturer = manufacturer;
    return descriptor;
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

} // namespace

class SerialSettingsPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-9: discovered ports, manual entry, missing saved port ------------
    void savedPortStaysSelectedWhenItIsPresentAmongDiscoveredPorts();
    void missingSavedPortIsVisiblyReportedAndNotSilentlyReplaced();
    void manualEntryStaysAvailableWithoutAnyDiscoveredPorts();

    // --- OB-9 / OB-10: explicit enumeration action only ----------------------
    void refreshControlEmitsEnumerationRequestOnlyOnExplicitAction();

    // --- OB-9: pending and terminal save states are visible ------------------
    void saveRequestCarriesTheCurrentSettings();
    void pendingAndTerminalSaveStatesAreVisiblyDistinguishable();

    // --- OB-6 / OB-9: duplicate save while pending is visibly rejected -------
    void duplicateSaveWhilePendingIsVisiblyRejectedWithoutASecondRequest();

    // --- OB-10: no request without an explicit administrator action -----------
    void noSaveOrEnumerationRequestWithoutAnExplicitAdministratorAction();
};

// --- OB-9 / OB-4 --------------------------------------------------------------

void SerialSettingsPageTest::savedPortStaysSelectedWhenItIsPresentAmongDiscoveredPorts()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    SerialConnectionSettings saved;
    saved.port_name = QStringLiteral("COM1");
    page.setSerialSettings(saved);

    page.setDiscoveredSerialPorts({descriptorFor(QStringLiteral("COM1"),
                                                 QStringLiteral("USB Serial"),
                                                 QStringLiteral("VendorA")),
                                   descriptorFor(QStringLiteral("COM3"))});

    QCOMPARE(page.serialPortComboBox()->count(), 2);
    QVERIFY(page.serialPortComboBox()->itemText(0).contains(QStringLiteral("COM1")));
    QVERIFY2(page.serialSettings().port_name == QStringLiteral("COM1"),
             "the saved port must remain selected when it is present among discovered ports");

    // Manual entry remains available next to the discovered list.
    QVERIFY(page.serialPortEdit()->isEnabled());
    page.serialPortEdit()->setText(QStringLiteral("COM42"));
    QCOMPARE(page.serialPortEdit()->text(), QStringLiteral("COM42"));
}

void SerialSettingsPageTest::missingSavedPortIsVisiblyReportedAndNotSilentlyReplaced()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    SerialConnectionSettings saved;
    saved.port_name = QStringLiteral("COM9");
    page.setSerialSettings(saved);
    const QString before = page.serialSettingsStatusText();

    page.setDiscoveredSerialPorts({descriptorFor(QStringLiteral("COM1")),
                                   descriptorFor(QStringLiteral("COM3"))});

    const QString after = page.serialSettingsStatusText();
    QVERIFY2(!after.trimmed().isEmpty() && after != before,
             "a saved port that is not among the discovered ports must be visibly reported");
    QVERIFY2(page.serialSettings().port_name == QStringLiteral("COM9"),
             "the missing saved port must not be silently replaced by a discovered port");
}

void SerialSettingsPageTest::manualEntryStaysAvailableWithoutAnyDiscoveredPorts()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setDiscoveredSerialPorts({});

    QCOMPARE(page.serialPortComboBox()->count(), 0);
    QVERIFY2(page.serialPortEdit()->isEnabled(),
             "manual entry must remain available regardless of the enumeration result");
    page.serialPortEdit()->setText(QStringLiteral("COM77"));
    QCOMPARE(page.serialPortEdit()->text(), QStringLiteral("COM77"));
}

// --- OB-9 / OB-10 -------------------------------------------------------------

void SerialSettingsPageTest::refreshControlEmitsEnumerationRequestOnlyOnExplicitAction()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy enumerationSpy(&page, &UsersSettingsPage::enumerateSerialPortsRequested);
    QCOMPARE(enumerationSpy.count(), 0);

    clickAt(page.refreshSerialPortsButton());
    QCOMPARE(enumerationSpy.count(), 1);
}

// --- OB-9 ---------------------------------------------------------------------

void SerialSettingsPageTest::saveRequestCarriesTheCurrentSettings()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    SerialConnectionSettings settings;
    settings.port_name = QStringLiteral("COM7");
    settings.station = 2;
    page.setSerialSettings(settings);

    QVector<SerialConnectionSettings> requests;
    connect(&page, &UsersSettingsPage::saveSerialSettingsRequested, this,
            [&requests](const SerialConnectionSettings &value) {
                requests.append(value);
            });

    clickAt(page.saveSerialSettingsButton());

    QCOMPARE(int(requests.size()), 1);
    QCOMPARE(requests[0].port_name, QStringLiteral("COM7"));
    QCOMPARE(int(requests[0].station), 2);
}

void SerialSettingsPageTest::pendingAndTerminalSaveStatesAreVisiblyDistinguishable()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QString idle = page.serialSettingsStatusText();

    page.setSerialSettingsSavePending();
    const QString pending = page.serialSettingsStatusText();
    QVERIFY2(!pending.trimmed().isEmpty() && pending != idle,
             "a pending save must be visible");
    QVERIFY2(!pending.contains(QStringLiteral("成功"))
                 && !pending.contains(QStringLiteral("已保存")),
             "a pending save must not claim success");

    page.setSerialSettingsSaveResult(true, QString());
    const QString succeeded = page.serialSettingsStatusText();
    QVERIFY2(!succeeded.trimmed().isEmpty() && succeeded != pending,
             "a terminal save result must be visible and distinguishable from pending");
    QVERIFY2(!succeeded.contains(QStringLiteral("失败")),
             "a successful save must not be presented as a failure");

    page.setSerialSettingsSaveResult(false, QStringLiteral("模拟数据库写入失败"));
    const QString failed = page.serialSettingsStatusText();
    QVERIFY2(failed.contains(QStringLiteral("模拟数据库写入失败")),
             "a failed save must surface the error detail");
    QVERIFY2(!failed.contains(QStringLiteral("已保存")),
             "a failed save must not claim success");
}

// --- OB-6 / OB-9 --------------------------------------------------------------

void SerialSettingsPageTest::duplicateSaveWhilePendingIsVisiblyRejectedWithoutASecondRequest()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QVector<SerialConnectionSettings> requests;
    connect(&page, &UsersSettingsPage::saveSerialSettingsRequested, this,
            [&requests](const SerialConnectionSettings &value) {
                requests.append(value);
            });

    clickAt(page.saveSerialSettingsButton());
    QCOMPARE(int(requests.size()), 1);

    // Application drives the page into the pending state while the save is in
    // flight; a second administrator click must not silently return.
    page.setSerialSettingsSavePending();
    const QString pending = page.serialSettingsStatusText();

    clickAt(page.saveSerialSettingsButton());

    QCOMPARE(int(requests.size()), 1);
    const QString rejection = page.serialSettingsStatusText();
    QVERIFY2(!rejection.trimmed().isEmpty() && rejection != pending,
             "a duplicate save while one is pending must be visibly rejected");

    // The rejection must not latch: after the terminal result the control is
    // usable again.
    page.setSerialSettingsSaveResult(true, QString());
    clickAt(page.saveSerialSettingsButton());
    QCOMPARE(int(requests.size()), 2);
}

// --- OB-10 --------------------------------------------------------------------

void SerialSettingsPageTest::noSaveOrEnumerationRequestWithoutAnExplicitAdministratorAction()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy enumerationSpy(&page, &UsersSettingsPage::enumerateSerialPortsRequested);
    QSignalSpy saveSpy(&page, &UsersSettingsPage::saveSerialSettingsRequested);

    // Presenting persisted settings, discovered ports and save states are
    // render-only operations; they must not start an enumeration or a save.
    SerialConnectionSettings settings;
    settings.port_name = QStringLiteral("COM7");
    page.setSerialSettings(settings);
    page.setDiscoveredSerialPorts({descriptorFor(QStringLiteral("COM1")),
                                   descriptorFor(QStringLiteral("COM3"))});
    page.setSerialSettingsSavePending();
    page.setSerialSettingsSaveResult(true, QString());
    QCoreApplication::processEvents();

    QCOMPARE(enumerationSpy.count(), 0);
    QCOMPARE(saveSpy.count(), 0);
}

QTEST_MAIN(SerialSettingsPageTest)
#include "serial_settings_page_test.moc"
