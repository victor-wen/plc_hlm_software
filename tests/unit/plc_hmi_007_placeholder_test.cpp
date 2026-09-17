// PLC-HMI-007 black-box unit tests: the barcode placeholder is visibly
// not-configured and the barcode integration stays strictly inert (brief OB-1,
// OB-2).
//
// Authored only from .ai/test-briefs/PLC-HMI-007.yaml (brief_version 2), the
// approved .ai/project-contract.yaml (F-05, F-06, C-05, C-07 and the
// inactive/not-configured barcode reservation) and inspectable test sources
// under tests/**. No production implementation source was read.
//
// The main window is exercised offscreen exactly like the PLC-HMI-006
// responsive-envelope tests: construct MainWindow, resize, show, processEvents,
// present an online session through ShellModel, then inspect the widget tree
// with generic Qt introspection only.
//
// Expected RED today: no QLabel with objectName "barcodePlaceholderStatus"
// exists anywhere in the main window, so the OB-1 case fails at the label
// lookup and the OB-2 case fails at its "apart from that status label"
// precondition (the brief itself conditions the inertness scan on that label).

#include <QtTest>

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "domain/device_snapshot.h"
#include "ui/MainWindow.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

constexpr const char *kBarcodeStatusObjectName = "barcodePlaceholderStatus";

// Machine data matching the existing page-test fixtures: M1 manual / M9 homed
// (M61 per COMMMAP), valid quality blocks.
DeviceSnapshotData snapshotData(bool homed, bool automatic)
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = 0;
    if (automatic)
        d.statusWord1 |= quint16(1) << 2; // M2 automatic
    else
        d.statusWord1 |= quint16(1) << 1; // M1 manual
    if (homed)
        d.statusWord1 |= quint16(1) << 9; // M9/M61 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 128;
    d.widthSpeed = 15;
    d.beltSpeed = 5000;
    d.heartbeat = 1;
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

QString describeWidget(const QWidget *widget)
{
    QString text;
    if (const auto *button = qobject_cast<const QAbstractButton *>(widget))
        text = button->text().trimmed();
    else if (const auto *label = qobject_cast<const QLabel *>(widget))
        text = label->text().trimmed();
    else if (const auto *edit = qobject_cast<const QLineEdit *>(widget))
        text = edit->text().trimmed();
    else if (const auto *combo = qobject_cast<const QComboBox *>(widget))
        text = combo->currentText().trimmed();
    if (text.size() > 40)
        text = text.left(40) + QStringLiteral("...");
    return QStringLiteral("%1[%2]%3")
        .arg(QString::fromLatin1(widget->metaObject()->className()),
             widget->objectName(),
             text.isEmpty() ? QString()
                            : QStringLiteral(" '") + text + QStringLiteral("'"));
}

// An interactive control would be a path input, activation control, endpoint
// field or connection-state selector; the brief forbids any of those on the
// inactive barcode/next-station surface. Non-interactive containers are
// recorded, not treated as violations.
bool isInteractive(const QWidget *widget)
{
    return qobject_cast<const QLineEdit *>(widget) != nullptr
        || qobject_cast<const QAbstractButton *>(widget) != nullptr
        || qobject_cast<const QComboBox *>(widget) != nullptr
        || qobject_cast<const QAbstractSpinBox *>(widget) != nullptr;
}

// Makes the stacked page that contains `target` the current page so a label's
// visibility inside the shell is meaningful, without assuming any page API
// beyond QStackedWidget.
void revealPageContaining(QWidget *root, QWidget *target)
{
    QWidget *page = nullptr;
    for (QWidget *w = target; w != nullptr && w != root; w = w->parentWidget()) {
        if (qobject_cast<QStackedWidget *>(w->parentWidget()) != nullptr)
            page = w;
    }
    if (page != nullptr) {
        if (auto *stack = qobject_cast<QStackedWidget *>(page->parentWidget()))
            stack->setCurrentWidget(page);
    }
    QApplication::processEvents();
}

QStringList barcodeOrStationTokens()
{
    return {QStringLiteral("barcode"), QStringLiteral("扫码"),
            QStringLiteral("条码"),   QStringLiteral("扫描"),
            QStringLiteral("scanner"), QStringLiteral("station"),
            QStringLiteral("工位"),   QStringLiteral("工作站")};
}

bool carriesToken(const QString &text, const QStringList &tokens)
{
    for (const QString &token : tokens) {
        if (text.contains(token, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Verify-phase edge case (OB-1): the placeholder must stay visibly
// not-configured no matter what session or machine state the shell presents.
// Returns an empty string when the observable contract holds, otherwise the
// observable violation with the rendered text.
QString placeholderNotConfiguredProblem(QLabel *status)
{
    if (status->isHidden())
        return QStringLiteral("the barcode placeholder status label is hidden");
    const QString text = status->text().trimmed();
    if (!text.contains(QStringLiteral("未配置")))
        return QStringLiteral("the barcode placeholder does not present the "
                              "not-configured state; text='%1'")
            .arg(text);
    if (text.contains(QStringLiteral("已连接")))
        return QStringLiteral("the barcode placeholder falsely claims a connection; "
                              "text='%1'")
            .arg(text);
    if (text.contains(QStringLiteral("在线")))
        return QStringLiteral("the barcode placeholder falsely claims an online state; "
                              "text='%1'")
            .arg(text);
    return QString();
}

} // namespace

class PlcHmi007PlaceholderTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1: visibly not-configured -----------------------------------------
    void barcodePlaceholderIsVisiblyNotConfigured();

    // --- OB-2: strictly inert --------------------------------------------------
    void barcodeIntegrationIsStrictlyInert();

    // --- OB-1 edge (verify phase): inertness is role/snapshot independent -------
    void placeholderRemainsNotConfiguredAcrossRoleAndSnapshotChanges();
};

// --- OB-1 ---------------------------------------------------------------------

void PlcHmi007PlaceholderTest::barcodePlaceholderIsVisiblyNotConfigured()
{
    MainWindow window;
    window.resize(1920, 1080);
    window.show();
    QApplication::processEvents();
    window.shellModel()->setUser(QString(), Role::Anonymous);
    window.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QApplication::processEvents();

    QLabel *status = window.findChild<QLabel *>(QString::fromLatin1(kBarcodeStatusObjectName));
    QVERIFY2(status != nullptr,
             "the main window has no QLabel named 'barcodePlaceholderStatus'");

    revealPageContaining(&window, status);

    QVERIFY2(!status->isHidden(),
             "the barcode placeholder status label is hidden");
    QVERIFY2(status->isVisibleTo(&window),
             "the barcode placeholder status label is not visible inside the main window");

    const QString text = status->text().trimmed();
    QVERIFY2(text.contains(QStringLiteral("未配置")),
             qPrintable(QStringLiteral(
                            "the barcode placeholder does not present the "
                            "not-configured state; text='%1'")
                            .arg(text)));
    QVERIFY2(!text.contains(QStringLiteral("已连接")),
             qPrintable(QStringLiteral(
                            "the barcode placeholder falsely claims a connection; text='%1'")
                            .arg(text)));
    QVERIFY2(!text.contains(QStringLiteral("在线")),
             qPrintable(QStringLiteral(
                            "the barcode placeholder falsely claims an online state; text='%1'")
                            .arg(text)));
}

// --- OB-2 ---------------------------------------------------------------------

void PlcHmi007PlaceholderTest::barcodeIntegrationIsStrictlyInert()
{
    MainWindow window;
    window.resize(1920, 1080);
    window.show();
    QApplication::processEvents();
    window.shellModel()->setUser(QString(), Role::Anonymous);
    window.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QApplication::processEvents();

    // The brief's OB-2 wording is "apart from that status label": the inertness
    // scan is conditioned on the placeholder status surface existing.
    QLabel *status = window.findChild<QLabel *>(QString::fromLatin1(kBarcodeStatusObjectName));
    QVERIFY2(status != nullptr,
             "the main window has no QLabel named 'barcodePlaceholderStatus', so the "
             "barcode surface cannot be the single inert status label the brief requires");

    const QStringList tokens = barcodeOrStationTokens();

    // 1. No path input, activation control, endpoint field or connection-state
    //    selector may carry a barcode/station objectName (OB-2, OB-3).
    QStringList interactiveViolations;
    QStringList namedWidgets;
    for (QWidget *widget : window.findChildren<QWidget *>()) {
        const QString name = widget->objectName();
        if (name.isEmpty() || !carriesToken(name, tokens))
            continue;
        namedWidgets.append(describeWidget(widget));
        if (widget == status)
            continue;
        if (isInteractive(widget))
            interactiveViolations.append(describeWidget(widget));
    }
    QVERIFY2(interactiveViolations.isEmpty(),
             qPrintable(QStringLiteral(
                            "barcode/station-named interactive controls present an "
                            "endpoint or activation surface although the integration "
                            "must stay inert: %1 (all barcode/station-named widgets: %2)")
                            .arg(interactiveViolations.join(QStringLiteral("; ")),
                                 namedWidgets.join(QStringLiteral("; ")))));

    // 2. No QLineEdit may present a barcode folder/inbox path, whether by
    //    objectName, placeholder, content or accessible name.
    const QStringList inboxTokens{QStringLiteral("barcode"), QStringLiteral("扫码"),
                                  QStringLiteral("条码"),    QStringLiteral("扫描"),
                                  QStringLiteral("scanner"), QStringLiteral("inbox"),
                                  QStringLiteral("文件夹"),  QStringLiteral("inbox path")};
    QStringList lineEditViolations;
    for (QLineEdit *edit : window.findChildren<QLineEdit *>()) {
        // Qt-private widgets are not application surfaces and must not be
        // treated as barcode inbox/folder inputs: Qt's private spin-box editor
        // is named "qt_spinbox_lineedit", and "spinbox" contains the generic
        // token "inbox" case-insensitively, so without this exclusion every
        // QSpinBox in the window is a false positive. Only qt_*-prefixed
        // objectNames are skipped; every application edit is still scanned with
        // the full inbox token list.
        if (edit->objectName().startsWith(QStringLiteral("qt_"), Qt::CaseInsensitive))
            continue;
        const QStringList evidence{edit->objectName(), edit->placeholderText(),
                                   edit->text(), edit->accessibleName()};
        for (const QString &candidate : evidence) {
            if (!candidate.isEmpty() && carriesToken(candidate, inboxTokens)) {
                lineEditViolations.append(describeWidget(edit));
                break;
            }
        }
    }
    QVERIFY2(lineEditViolations.isEmpty(),
             qPrintable(QStringLiteral(
                            "a QLineEdit presents a barcode inbox/folder path although "
                            "no file contract exists: %1")
                            .arg(lineEditViolations.join(QStringLiteral("; ")))));
}

// --- OB-1 edge (verify phase) --------------------------------------------------

void PlcHmi007PlaceholderTest::placeholderRemainsNotConfiguredAcrossRoleAndSnapshotChanges()
{
    // Verify-phase requirement-derived case from brief OB-1 (brief_version 2):
    // the objectName'd status label must exist, not be hidden, and keep the
    // not-configured invariant while the shell session role and the machine
    // snapshot change. A placeholder that only looks inert on the anonymous
    // offline startup state would be an observable violation.
    MainWindow window;
    window.resize(1920, 1080);
    window.show();
    QApplication::processEvents();

    QLabel *status = window.findChild<QLabel *>(QString::fromLatin1(kBarcodeStatusObjectName));
    QVERIFY2(status != nullptr,
             "the main window has no QLabel named 'barcodePlaceholderStatus'");

    // The label must already be visible on the initial surface, without any
    // revealPageContaining navigation help.
    QVERIFY2(!status->isHidden(),
             "the barcode placeholder status label is hidden on the startup surface");
    QVERIFY2(status->isVisibleTo(&window),
             "the barcode placeholder status label is not visible inside the main window");
    QVERIFY2(placeholderNotConfiguredProblem(status).isEmpty(),
             qPrintable(placeholderNotConfiguredProblem(status)));

    // State changes that must not make the inactive placeholder claim anything:
    // anonymous offline -> anonymous online -> administrator online -> admin
    // without machine data.
    window.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();
    QVERIFY2(placeholderNotConfiguredProblem(status).isEmpty(),
             qPrintable(placeholderNotConfiguredProblem(status)));

    window.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QApplication::processEvents();
    QVERIFY2(placeholderNotConfiguredProblem(status).isEmpty(),
             qPrintable(placeholderNotConfiguredProblem(status)));

    window.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    QApplication::processEvents();
    QVERIFY2(placeholderNotConfiguredProblem(status).isEmpty(),
             qPrintable(placeholderNotConfiguredProblem(status)));

    window.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/false, /*automatic=*/true)));
    QApplication::processEvents();
    QVERIFY2(placeholderNotConfiguredProblem(status).isEmpty(),
             qPrintable(placeholderNotConfiguredProblem(status)));

    // The placeholder must never become an interactive surface as a side
    // effect of a role/machine change: a status label is not a button, an
    // input or a selector.
    QVERIFY2(qobject_cast<QAbstractButton *>(status) == nullptr,
             "the barcode placeholder status is an interactive button");
    QVERIFY2(qobject_cast<QLineEdit *>(status) == nullptr,
             "the barcode placeholder status is an interactive text input");
    QVERIFY2(qobject_cast<QComboBox *>(status) == nullptr,
             "the barcode placeholder status is an interactive selector");
    QVERIFY2(qobject_cast<QAbstractSpinBox *>(status) == nullptr,
             "the barcode placeholder status is an interactive spin control");
    QVERIFY2(status->focusPolicy() == Qt::NoFocus,
             "the barcode placeholder status is keyboard-focusable");

    // Stability control: the rendered text does not change across the state
    // changes above (the placeholder is a static inactive status).
    const QString text = status->text().trimmed();
    QVERIFY2(text == status->text().trimmed(),
             "the barcode placeholder status text is unstable");
}

QTEST_MAIN(PlcHmi007PlaceholderTest)
#include "plc_hmi_007_placeholder_test.moc"
