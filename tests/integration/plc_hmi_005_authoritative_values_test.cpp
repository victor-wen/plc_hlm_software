// PLC-HMI-005 black-box integration tests through the real composition root:
// independent per-field parameter validation without the obsolete product gate
// (OB-4), a visible touch-readable reliability disclosure for the D138/D139
// production count (OB-5), simulator heartbeat-freeze convergence of pending
// submissions with recovery (OB-7), and no regression of unrelated visible
// state, command lifecycle and restricted-mode behavior (OB-8).
//
// Authored only from .ai/test-briefs/PLC-HMI-005.yaml, the approved
// .ai/project-contract.yaml and inspectable test sources under tests/**. No
// production implementation source was read.
//
// Frozen existing surface used (all symbols below appear in inspectable test
// sources under tests/**): Application/AppConfig, SimulatedPlcGateway
// (model/tick/isOnline/setHeartbeatFrozen), UsersSettingsPage (d204Spin,
// writeD204Button, paramStatusText, d204WriteRequested), AdminPasswordDialog,
// OverviewPage, ShellModel/OperatorCommandStatus, MainWindow.
//
// Expected RED: runtime assertion failures once registered (the settings page
// still enforces the D204*D220 product gate and no reliability disclosure is
// rendered for the production count).

#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include <QThread>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "app/lifecycle_controller.h"
#include "domain/device_snapshot.h"
#include "domain/operator_command_status.h"
#include "ui/MainWindow.h"
#include "ui/dialogs/admin_password_dialog.h"
#include "ui/pages/overview_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM43 = 43;
constexpr quint16 kM103 = 103;
constexpr quint16 kM50 = 50; // PLC-HMI-011: home-start coil
constexpr quint16 kM104 = 104;
constexpr quint16 kD128 = 128;
constexpr quint16 kD138 = 138;
constexpr quint16 kD139 = 139;
constexpr quint16 kD204 = 204;
constexpr quint16 kD220 = 220;

// Legible touch text minimum used by the earlier independent UI tests; the
// contract fixes no exact size.
constexpr int kMinimumLegibleFontHeight = 12;

Application *startSimulatedApplication(const QTemporaryDir &dir,
                                       SimulatedPlcGateway **gwOut)
{
    auto *cfg = new AppConfig;
    cfg->useSimulatedGateway = true;
    cfg->simulatedTickIntervalMs = 0;
    cfg->databasePath = dir.filePath(QStringLiteral("app.db"));

    auto *app = new Application(*cfg);
    delete cfg;

    app->start();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    if (gwOut != nullptr)
        *gwOut = gw;
    return app;
}

void advanceUntilOnline(SimulatedPlcGateway &gw, int maxTicks = 10)
{
    for (int i = 0; i < maxTicks && !gw.isOnline(); ++i)
        gw.tick();
}

void homeReady(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.model().writeCoil(kM50, true); // PLC-HMI-011: homing starts on the home-start write
    gw.tick();
    gw.tick(); // home return takes 2 s
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

DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210
    d.pulsePerMm = 128;    // D204
    d.widthSpeed = 15;     // D220
    d.beltSpeed = 5000;    // D122
    d.heartbeat = 1;       // D140
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

QVector<QLabel *> visibleTextLabels(QWidget *root)
{
    QVector<QLabel *> labels;
    for (QLabel *label : root->findChildren<QLabel *>()) {
        const QString text = label->text().trimmed();
        if (text.isEmpty())
            continue;
        if (label->isHidden())
            continue;
        if (!label->isVisibleTo(root))
            continue;
        labels.append(label);
    }
    return labels;
}

QStringList visibleTexts(QWidget *root)
{
    QStringList texts;
    for (QLabel *label : visibleTextLabels(root)) {
        const QString text = label->text().trimmed();
        if (!texts.contains(text))
            texts.append(text);
    }
    return texts;
}

bool containsReliabilityNegation(const QString &text)
{
    const QStringList tokens{
        QStringLiteral("不可靠"), QStringLiteral("可能不"),
        QStringLiteral("失真"),   QStringLiteral("不保证"),
        QStringLiteral("仅供参考"), QStringLiteral("不准确"),
        QStringLiteral("偏差"),   QStringLiteral("异常"),
    };
    for (const QString &token : tokens) {
        if (text.contains(token))
            return true;
    }
    return false;
}

// A reliability disclosure for the production count must combine an
// unreliability statement with the automatic width adjustment cause. The
// contract fixes the requirement, not the exact wording.
bool isReliabilityDisclosure(const QString &text)
{
    const QStringList adjustmentTokens{
        QStringLiteral("调宽"), QStringLiteral("宽度调整"),
        QStringLiteral("自动调整"),
    };
    bool mentionsAdjustment = false;
    for (const QString &token : adjustmentTokens) {
        if (text.contains(token)) {
            mentionsAdjustment = true;
            break;
        }
    }
    return mentionsAdjustment && containsReliabilityNegation(text);
}

struct CountDisclosureObservation
{
    bool countShown = false;
    bool disclosureVisible = false;
};

// Observes the overview surface for a production-count token and for a
// reliability disclosure in the same visible text set.
CountDisclosureObservation observeCountDisclosure(QWidget *root,
                                                  const QString &countToken)
{
    CountDisclosureObservation out;
    for (const QString &text : visibleTexts(root)) {
        if (text.contains(countToken))
            out.countShown = true;
        if (isReliabilityDisclosure(text))
            out.disclosureVisible = true;
    }
    return out;
}

} // namespace

class PlcHmi005AuthoritativeValuesTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-4: independent per-field parameter validation ---------------------
    void parameterValidationIsIndependentPerFieldWithoutTheProductGate();
    void minimumD204WithMaximumD220DispatchesWithoutTheProductGate();

    // --- OB-5: production count reliability disclosure ------------------------
    void productionCountCarriesAVisibleTouchReadableReliabilityDisclosure();
    void reliabilityDisclosureRemainsVisibleAfterProductionCountUpdates();

    // --- OB-7: heartbeat-freeze convergence and recovery ----------------------
    void heartbeatFreezeConvergesPendingSubmissionAndRecovers();
    void heartbeatFreezeConvergenceRepeatsAcrossASecondLossCycle();

    // --- OB-8: unrelated visible behavior remains unchanged --------------------
    void healthySessionDecodesAuthoritativeRegistersConsistently();
    void commandLifecycleStillConvergesVisibly();
    void restrictedModeStillBlocksNonSafetyCommands();
};

// --- OB-4 ---------------------------------------------------------------------

void PlcHmi005AuthoritativeValuesTest::parameterValidationIsIndependentPerFieldWithoutTheProductGate()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // D204 is validated against its decoded range (contract: 1..32767).
    QCOMPARE(page.d204Spin()->minimum(), 1);
    QCOMPARE(page.d204Spin()->maximum(), 32767);

    // D220 is validated against 1..15 independently. The D220 editor is
    // identified by its decoded range, not by a guessed accessor name.
    QVector<QSpinBox *> d220Candidates;
    for (QSpinBox *spin : page.findChildren<QSpinBox *>()) {
        if (spin == page.d204Spin())
            continue;
        if (spin->maximum() == 15)
            d220Candidates.append(spin);
    }
    QVERIFY2(!d220Candidates.isEmpty(),
             "no D220 editor with the decoded 1..15 range was found on the settings page");
    for (QSpinBox *candidate : d220Candidates) {
        QCOMPARE(candidate->minimum(), 1);
        QCOMPARE(candidate->maximum(), 15);
    }
    QSpinBox *const d220 = d220Candidates.first();

    // A valid D204/D220 combination whose product is extreme must not be
    // rejected by any D204*D220 product gate: only the fields' own ranges may
    // gate the write.
    QSignalSpy writeSpy(&page, &UsersSettingsPage::d204WriteRequested);
    page.d204Spin()->setValue(32767);
    d220->setValue(15);

    clickAt(page.writeD204Button());
    auto *dialog = page.findChild<AdminPasswordDialog *>();
    QVERIFY2(dialog != nullptr,
             "the D204 write flow did not open the administrator confirmation dialog");
    dialog->passwordEdit()->setText(QStringLiteral("admin-secret"));
    clickAt(dialog->okButton());

    QVERIFY2(writeSpy.count() == 1,
             qPrintable(QStringLiteral("the independent D204=32767/D220=15 write was "
                                       "not dispatched (status=%1)")
                            .arg(page.paramStatusText())));
    QVERIFY2(!page.paramStatusText().contains(QStringLiteral("乘积")),
             "the parameter reason mentions the obsolete D204*D220 product");
}

void PlcHmi005AuthoritativeValuesTest::minimumD204WithMaximumD220DispatchesWithoutTheProductGate()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // The lowest valid D204 with the highest valid D220 is a boundary pair
    // whose product is small; per-field validated values must still be
    // dispatched with no product gate.
    page.d204Spin()->setValue(1);
    QVector<QSpinBox *> d220Candidates;
    for (QSpinBox *spin : page.findChildren<QSpinBox *>()) {
        if (spin == page.d204Spin())
            continue;
        if (spin->maximum() == 15)
            d220Candidates.append(spin);
    }
    QVERIFY2(!d220Candidates.isEmpty(),
             "no D220 editor with the decoded 1..15 range was found");
    d220Candidates.first()->setValue(15);

    QSignalSpy writeSpy(&page, &UsersSettingsPage::d204WriteRequested);
    clickAt(page.writeD204Button());
    auto *dialog = page.findChild<AdminPasswordDialog *>();
    QVERIFY2(dialog != nullptr,
             "the D204=1/D220=15 write flow did not open the administrator "
             "confirmation dialog");
    dialog->passwordEdit()->setText(QStringLiteral("admin-secret"));
    clickAt(dialog->okButton());

    QVERIFY2(writeSpy.count() == 1,
             qPrintable(QStringLiteral("the independent D204=1/D220=15 write was "
                                       "not dispatched (status=%1)")
                            .arg(page.paramStatusText())));
    QVERIFY2(!page.paramStatusText().contains(QStringLiteral("乘积")),
             "the parameter reason mentions the obsolete D204*D220 product");
}

// --- OB-5 ---------------------------------------------------------------------

void PlcHmi005AuthoritativeValuesTest::productionCountCarriesAVisibleTouchReadableReliabilityDisclosure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    app->window()->show();
    QApplication::processEvents();

    // Put a distinctive production count into the D138/D139 registers the HMI
    // reads, then let the shell publish the decoded snapshot.
    gw->model().writeRegister(kD138, 4242);
    gw->model().writeRegister(kD139, 0);
    for (int i = 0; i < 3; ++i)
        gw->tick();
    QApplication::processEvents();

    OverviewPage *overview = app->window()->findChild<OverviewPage *>();
    QVERIFY2(overview != nullptr, "the composed shell has no overview page");

    QStringList texts = visibleTexts(overview);
    QVERIFY2(!texts.isEmpty(), "precondition: the overview page shows no visible text");

    bool countShown = false;
    for (const QString &text : texts) {
        if (text.contains(QStringLiteral("4242"))) {
            countShown = true;
            break;
        }
    }
    QVERIFY2(countShown,
             "the D138/D139 production count is not displayed on the overview surface");

    bool disclosureVisible = false;
    for (const QString &text : texts) {
        if (isReliabilityDisclosure(text)) {
            disclosureVisible = true;
            break;
        }
    }
    QVERIFY2(disclosureVisible,
             "the production count is displayed without a visible reliability "
             "disclosure (unreliable after automatic width adjustment)");

    // The disclosure must be real, non-hidden, legible text next to the count,
    // not tooltip-only and not hover-dependent.
    QLabel *disclosureLabel = nullptr;
    for (QLabel *label : visibleTextLabels(overview)) {
        if (isReliabilityDisclosure(label->text().trimmed())) {
            disclosureLabel = label;
            break;
        }
    }
    QVERIFY2(disclosureLabel != nullptr, "no visible disclosure label was found");
    QVERIFY(!disclosureLabel->isHidden());
    QVERIFY2(disclosureLabel->fontMetrics().height() >= kMinimumLegibleFontHeight,
             "the reliability disclosure text is below the legible touch minimum");

    // The value must never be presented as reliable without the disclosure.
    for (const QString &text : texts) {
        const bool countRelated = text.contains(QStringLiteral("4242"))
            || text.contains(QStringLiteral("产量"))
            || text.contains(QStringLiteral("生产"))
            || text.contains(QStringLiteral("计数"));
        if (!countRelated || !text.contains(QStringLiteral("可靠")))
            continue;
        QVERIFY2(containsReliabilityNegation(text),
                 qPrintable(QStringLiteral("production count presented as reliable "
                                           "without a negation: %1")
                                .arg(text)));
    }

    app->shutdown();
}

void PlcHmi005AuthoritativeValuesTest::reliabilityDisclosureRemainsVisibleAfterProductionCountUpdates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    app->window()->show();
    QApplication::processEvents();

    gw->model().writeRegister(kD138, 4242);
    gw->model().writeRegister(kD139, 0);
    for (int i = 0; i < 3; ++i)
        gw->tick();
    QApplication::processEvents();

    OverviewPage *overview = app->window()->findChild<OverviewPage *>();
    QVERIFY2(overview != nullptr, "the composed shell has no overview page");

    const CountDisclosureObservation before =
        observeCountDisclosure(overview, QStringLiteral("4242"));
    QVERIFY2(before.countShown,
             "precondition: the initial production count is not displayed");
    QVERIFY2(before.disclosureVisible,
             "precondition: no reliability disclosure is visible with the initial count");

    // The production count advances while the machine condition is unchanged:
    // the disclosure obligation must survive the update.
    gw->model().writeRegister(kD138, 7777);
    gw->model().writeRegister(kD139, 0);
    for (int i = 0; i < 3; ++i)
        gw->tick();
    QApplication::processEvents();

    const CountDisclosureObservation after =
        observeCountDisclosure(overview, QStringLiteral("7777"));
    QVERIFY2(after.countShown, "the updated production count is not displayed");
    QVERIFY2(after.disclosureVisible,
             "the reliability disclosure disappeared after the production count changed");

    // The disclosure must remain real, non-hidden and legible after the update,
    // not tooltip-only and not hover-dependent.
    QLabel *disclosureLabel = nullptr;
    for (QLabel *label : visibleTextLabels(overview)) {
        if (isReliabilityDisclosure(label->text().trimmed())) {
            disclosureLabel = label;
            break;
        }
    }
    QVERIFY2(disclosureLabel != nullptr,
             "no visible disclosure label was found after the production count update");
    QVERIFY(!disclosureLabel->isHidden());
    QVERIFY2(!disclosureLabel->isHidden() && disclosureLabel->isVisibleTo(overview),
             "the disclosure label is not visible after the production count update");
    QVERIFY2(disclosureLabel->fontMetrics().height() >= kMinimumLegibleFontHeight,
             "the reliability disclosure text is below the legible touch minimum "
             "after the production count update");

    // The updated value must never be presented as reliable without the
    // disclosure.
    for (const QString &text : visibleTexts(overview)) {
        const bool countRelated = text.contains(QStringLiteral("7777"))
            || text.contains(QStringLiteral("产量"))
            || text.contains(QStringLiteral("生产"))
            || text.contains(QStringLiteral("计数"));
        if (!countRelated || !text.contains(QStringLiteral("可靠")))
            continue;
        QVERIFY2(containsReliabilityNegation(text),
                 qPrintable(QStringLiteral("the updated production count is "
                                           "presented as reliable without a "
                                           "negation: %1")
                                .arg(text)));
    }

    app->shutdown();
}

// --- OB-7 ---------------------------------------------------------------------

void PlcHmi005AuthoritativeValuesTest::heartbeatFreezeConvergesPendingSubmissionAndRecovers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    app->coordinator()->setRole(Role::Admin);
    homeReady(*gw);

    // A stalled positioning keeps the adjust pending until the link converges.
    gw->model().setPositioningStall(true);
    QVERIFY2(app->coordinator()->adjustWidth(300).accepted,
             "precondition: the pending adjust was not accepted");
    QVERIFY(app->coordinator()->adjustInProgress());

    QVector<bool> connectionChanges;
    connect(gw, &SimulatedPlcGateway::connectionStateChanged, this,
            [&connectionChanges](quint64, bool online) {
                connectionChanges.append(online);
            });

    // The heartbeat freeze must converge the pending submission like the real
    // gateway's offline convergence.
    gw->setHeartbeatFrozen(true);
    bool terminal = false;
    int ticks = 0;
    for (; ticks < 20 && !terminal; ++ticks) {
        gw->tick();
        terminal = isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(terminal,
             "a frozen heartbeat did not converge the pending submission within 20 s");

    const OperatorCommandStatus status = app->shell()->operatorCommandStatus();
    QVERIFY2(status.lifecycle_state != OperatorCommandState::Succeeded,
             "a communications loss must not be reported as machine success");
    QVERIFY2(!status.human_readable_detail.isEmpty(),
             "the converged state must carry a visible detail");
    QVERIFY2(status.lifecycle_state == OperatorCommandState::CommunicationsLost
                 || status.lifecycle_state == OperatorCommandState::GatewayReplaced
                 || status.human_readable_detail.contains(QStringLiteral("通信"))
                 || status.human_readable_detail.contains(QStringLiteral("离线"))
                 || status.human_readable_detail.contains(QStringLiteral("连接")),
             qPrintable(QStringLiteral("the pending submission converged without a "
                                       "communications-lost/replaced detail: %1")
                            .arg(status.human_readable_detail)));
    QVERIFY2(!app->coordinator()->adjustInProgress(),
             "the pending adjust stayed in progress after convergence");
    QVERIFY2(!connectionChanges.isEmpty() && connectionChanges.last() == false,
             "the heartbeat freeze was not observable as a connection state change");

    // Recovery: a resumed heartbeat restores the online session.
    gw->setHeartbeatFrozen(false);
    for (int i = 0; i < 20 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY2(gw->isOnline(), "the session did not recover after the heartbeat resumed");
    for (int i = 0; i < 5 && gw->lastSnapshot().overall_quality != DataQuality::Valid; ++i)
        gw->tick();
    QVERIFY2(gw->lastSnapshot().overall_quality == DataQuality::Valid,
             "the recovered session did not republish valid data");

    gw->model().setPositioningStall(false);
    const ControlCoordinator::CommandResult recovered = app->coordinator()->adjustWidth(350);
    QVERIFY2(recovered.accepted || !recovered.reason.isEmpty(),
             "the recovered session returned silently from a new command");
    bool recoveredTerminal = false;
    for (int i = 0; i < 25 && !recoveredTerminal; ++i) {
        gw->tick();
        recoveredTerminal =
            isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(recoveredTerminal, "the recovered command did not converge");
    QVERIFY2(app->shell()->operatorCommandStatus().lifecycle_state
                 != OperatorCommandState::CommunicationsLost,
             "the recovered session reported a communications loss again");

    app->shutdown();
}

void PlcHmi005AuthoritativeValuesTest::heartbeatFreezeConvergenceRepeatsAcrossASecondLossCycle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    app->coordinator()->setRole(Role::Admin);
    homeReady(*gw);

    QVector<bool> connectionChanges;
    connect(gw, &SimulatedPlcGateway::connectionStateChanged, this,
            [&connectionChanges](quint64, bool online) {
                connectionChanges.append(online);
            });

    // Cycle 1: a stalled pending adjust converges when the heartbeat freezes.
    gw->model().setPositioningStall(true);
    QVERIFY2(app->coordinator()->adjustWidth(300).accepted,
             "cycle 1: the pending adjust was not accepted");
    QVERIFY(app->coordinator()->adjustInProgress());

    gw->setHeartbeatFrozen(true);
    bool cycle1Terminal = false;
    for (int i = 0; i < 20 && !cycle1Terminal; ++i) {
        gw->tick();
        cycle1Terminal = isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(cycle1Terminal,
             "cycle 1: the frozen heartbeat did not converge the pending submission");
    const OperatorCommandStatus cycle1 = app->shell()->operatorCommandStatus();
    QVERIFY2(cycle1.lifecycle_state != OperatorCommandState::Succeeded,
             "cycle 1: a communications loss was reported as machine success");
    QVERIFY2(!cycle1.human_readable_detail.isEmpty(),
             "cycle 1: the converged state carried no visible detail");
    QVERIFY2(!app->coordinator()->adjustInProgress(),
             "cycle 1: the pending adjust stayed in progress after convergence");
    QVERIFY2(!connectionChanges.isEmpty() && connectionChanges.last() == false,
             "cycle 1: the freeze was not observable as an offline connection change");

    gw->setHeartbeatFrozen(false);
    for (int i = 0; i < 20 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY2(gw->isOnline(), "cycle 1: the session did not recover after the heartbeat resumed");
    for (int i = 0; i < 5 && gw->lastSnapshot().overall_quality != DataQuality::Valid; ++i)
        gw->tick();
    QVERIFY2(gw->lastSnapshot().overall_quality == DataQuality::Valid,
             "cycle 1: the recovered session did not republish valid data");

    // Let any interrupted in-flight adjust settle before the second cycle.
    gw->model().setPositioningStall(false);
    for (int i = 0; i < 35 && app->coordinator()->adjustInProgress(); ++i)
        gw->tick();
    QVERIFY2(!app->coordinator()->adjustInProgress(),
             "cycle 1: the interrupted adjust did not settle before cycle 2");

    // Cycle 2: the same freeze/converge/recover sequence must behave the same.
    gw->model().setPositioningStall(true);
    QVERIFY2(app->coordinator()->adjustWidth(260).accepted,
             "cycle 2: the second pending adjust was not accepted");
    QVERIFY(app->coordinator()->adjustInProgress());

    gw->setHeartbeatFrozen(true);
    bool cycle2Terminal = false;
    for (int i = 0; i < 20 && !cycle2Terminal; ++i) {
        gw->tick();
        cycle2Terminal = isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(cycle2Terminal,
             "cycle 2: the second frozen heartbeat did not converge the pending submission");
    const OperatorCommandStatus cycle2 = app->shell()->operatorCommandStatus();
    QVERIFY2(cycle2.lifecycle_state != OperatorCommandState::Succeeded,
             "cycle 2: a communications loss was reported as machine success");
    QVERIFY2(!cycle2.human_readable_detail.isEmpty(),
             "cycle 2: the converged state carried no visible detail");
    QVERIFY2(!app->coordinator()->adjustInProgress(),
             "cycle 2: the pending adjust stayed in progress after convergence");
    QVERIFY2(!connectionChanges.isEmpty() && connectionChanges.last() == false,
             "cycle 2: the second freeze was not observable as an offline connection change");

    gw->setHeartbeatFrozen(false);
    for (int i = 0; i < 20 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY2(gw->isOnline(), "cycle 2: the session did not recover after the heartbeat resumed");
    for (int i = 0; i < 5 && gw->lastSnapshot().overall_quality != DataQuality::Valid; ++i)
        gw->tick();
    QVERIFY2(gw->lastSnapshot().overall_quality == DataQuality::Valid,
             "cycle 2: the recovered session did not republish valid data");

    // After two converged loss cycles the session still accepts a command and
    // converges it without another communications loss.
    gw->model().setPositioningStall(false);
    const ControlCoordinator::CommandResult followUp = app->coordinator()->adjustWidth(120);
    QVERIFY2(followUp.accepted || !followUp.reason.isEmpty(),
             "the twice-recovered session returned silently from a new command");
    bool followUpTerminal = false;
    for (int i = 0; i < 25 && !followUpTerminal; ++i) {
        gw->tick();
        followUpTerminal = isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(followUpTerminal, "the command after two loss cycles did not converge");
    QVERIFY2(app->shell()->operatorCommandStatus().lifecycle_state
                 != OperatorCommandState::CommunicationsLost,
             "the twice-recovered session reported another communications loss");

    app->shutdown();
}

// --- OB-8 ---------------------------------------------------------------------

void PlcHmi005AuthoritativeValuesTest::healthySessionDecodesAuthoritativeRegistersConsistently()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    for (int i = 0; i < 5; ++i)
        gw->tick();

    const DeviceSnapshot snap = gw->lastSnapshot();
    QVERIFY2(snap.overall_quality == DataQuality::Valid,
             "a healthy session must publish valid data");
    QVERIFY(snap.overall_age_ms >= 0);
    QCOMPARE(int(snap.beltSpeed()), int(gw->model().readRegister(122)));
    QCOMPARE(int(snap.pulsePerMm()), int(gw->model().readRegister(kD204)));
    QCOMPARE(int(snap.widthSpeed()), int(gw->model().readRegister(kD220)));
    QCOMPARE(int(snap.targetWidth()), int(gw->model().readRegister(kD128)));
    QCOMPARE(int(snap.currentWidth()), int(gw->model().readRegister(130)));

    app->shutdown();
}

void PlcHmi005AuthoritativeValuesTest::commandLifecycleStillConvergesVisibly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    app->coordinator()->setRole(Role::Admin);
    homeReady(*gw);

    QVERIFY2(app->coordinator()->reset().accepted,
             "an administrator reset on a manual homed machine must be accepted");

    bool terminal = false;
    for (int i = 0; i < 20 && !terminal; ++i) {
        gw->tick();
        terminal = isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(terminal, "the accepted reset did not converge to a terminal state");
    QVERIFY2(!app->shell()->operatorCommandStatus().human_readable_detail.isEmpty(),
             "the terminal command state must carry a visible detail");

    app->shutdown();
}

void PlcHmi005AuthoritativeValuesTest::restrictedModeStillBlocksNonSafetyCommands()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("app.db"));
    {
        QFile file(databasePath);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 "the test could not create the unusable database file");
        file.write("This file is not an SQLite database.");
        file.close();
    }

    auto *cfg = new AppConfig;
    cfg->useSimulatedGateway = true;
    cfg->simulatedTickIntervalMs = 0;
    cfg->databasePath = databasePath;
    std::unique_ptr<Application> app(new Application(*cfg));
    delete cfg;
    app->start();

    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    app->coordinator()->setRole(Role::Admin);
    const quint16 d128Before = gw->model().readRegister(kD128);

    // The database-restricted verdict is emitted on the database worker thread
    // and delivered as a queued signal, so it must be pumped into the main
    // thread before this case can observe restricted mode. Bounded, so a
    // regression can never hang the case. (2026-09-21: the case previously
    // passed without ever reaching restricted mode, because the adjust was
    // already rejected by the then-required 未回原点 interlock.)
    for (int i = 0; i < 400 && !app->lifecycle()->restricted(); ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    QVERIFY2(app->lifecycle()->restricted(),
             "precondition: the unusable database must put the application into restricted mode");

    const ControlCoordinator::CommandResult blocked = app->coordinator()->adjustWidth(300);
    QVERIFY2(!blocked.accepted,
             "restricted mode must keep rejecting non-safety commands");
    QVERIFY2(!blocked.reason.isEmpty(),
             "a restricted-mode rejection must carry a visible reason");
    QCOMPARE(int(gw->model().readRegister(kD128)), int(d128Before));

    app->shutdown();
}

QTEST_MAIN(PlcHmi005AuthoritativeValuesTest)
#include "plc_hmi_005_authoritative_values_test.moc"
