// PLC-HMI-001 black-box tests: operator-command status projection into the
// persistent shell surface (brief OB-4, OB-5 pending-detail contract, OB-9).
//
// This file exercises the contract API that does not exist before the change:
//   hlm::OperatorCommandStatus / isTerminal / toString
//   ShellModel::setOperatorCommandStatus / operatorCommandStatus /
//   clearOperatorCommandStatus / operatorCommandStatusChanged
//   ActionBar::commandStatusLabel
//
// A compile failure of this target before implementation is the expected RED
// for these behaviors (brief acceptance_criteria); it is deliberately isolated
// in this file so the existing-API targets can still build and run.
//
// PLC-HMI-011 OB-7 extension (authored from the behavior-only brief
// .ai/test-briefs/PLC-HMI-011.yaml and inspectable test sources only): the
// manual-control surface must show the new split - belt jog and stop gate are
// enabled once homing is complete, while the width jogs stay disabled with a
// visible reason until it is. No production implementation source was read.

#include <QtTest>

#include <QAbstractButton>
#include <QLabel>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "domain/device_snapshot.h"
#include "domain/operator_command_status.h"
#include "ui/pages/manual_control_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;                                     // M34/M44/M45 clear
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210
    d.pulsePerMm = 1280;   // D204
    d.widthSpeed = 15;     // D220
    d.heartbeat = 1;       // D140
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

OperatorCommandStatus makeStatus(OperatorCommandState state, const QString &detail,
                                 Command command = Command::Reset)
{
    OperatorCommandStatus s;
    s.command = command;
    s.lifecycle_state = state;
    s.human_readable_detail = detail;
    return s;
}

// --- PLC-HMI-011 OB-7: manual-control surface fixture --------------------------
//
// A ready manual session: online (connected, all blocks Valid), administrator,
// manual mode (M1), not running (M3 clear), no emergency stop (M0 clear), no
// latched fault (M14 clear), no width-adjust bits (M34/M44/M45 clear). The two
// homing flags are the only variable: home-complete (M9/M61) and the
// home-in-progress/home-start bit (M50, carried by homeBits bit 0).
DeviceSnapshotData manualSurfaceData(bool homeComplete, bool homeInProgress = false)
{
    DeviceSnapshotData d;
    d.connected = true;
    quint16 sw1 = 0;
    sw1 |= quint16(1) << 1; // M1 manual
    if (homeComplete)
        sw1 |= quint16(1) << 9; // M9/M61 home complete
    d.statusWord1 = sw1;
    d.statusWord3 = 0;
    d.homeBits = homeInProgress ? quint16(1) : quint16(0); // M50
    d.faultCode = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 128;
    d.widthSpeed = 15;
    d.beltSpeed = 5000;
    d.heartbeat = 1;
    d.fast_quality = DataQuality::Valid;
    d.fast_age_ms = 0;
    d.home_quality = DataQuality::Valid;
    d.home_age_ms = 0;
    d.command_quality = DataQuality::Valid;
    d.command_age_ms = 0;
    d.slow_quality = DataQuality::Valid;
    d.slow_age_ms = 0;
    d.overall_quality = aggregateQuality(d);
    return d;
}

// The reason a disabled control declares, independent of any production API:
// tooltip, status tip or accessible description (the PLC-HMI-008 pattern).
QString declaredReason(const QWidget *control)
{
    const QStringList candidates{control->toolTip(), control->statusTip(),
                                 control->accessibleDescription()};
    for (const QString &candidate : candidates) {
        if (!candidate.trimmed().isEmpty())
            return candidate.trimmed();
    }
    return QString();
}

// Visible labels on the presented page that carry the control's declared
// reason. A label whose whole text is contained in the declared reason (>= 4
// characters) also counts, so the implementation may shorten the wording
// without losing the semantics. In addition, a visible non-empty label inside
// the control itself counts as an inline reason (the PLC-HMI-008
// PermissionButton pattern draws its reason as a child label, and the brief
// requires a visible reason, not a specific channel). Tooltip-only explanation
// yields none of these.
QVector<QLabel *> visibleReasonLabels(QWidget *root, QWidget *control)
{
    QVector<QLabel *> labels;
    const QString reason = declaredReason(control);
    if (!reason.isEmpty()) {
        for (QLabel *label : root->findChildren<QLabel *>()) {
            const QString text = label->text().trimmed();
            if (text.isEmpty() || label->isHidden() || !label->isVisibleTo(root))
                continue;
            if (text.contains(reason) || (text.size() >= 4 && reason.contains(text)))
                labels.append(label);
        }
    }
    for (QLabel *label : control->findChildren<QLabel *>()) {
        if (label->text().trimmed().isEmpty() || label->isHidden())
            continue;
        if (!label->isVisibleTo(root))
            continue;
        if (!labels.contains(label))
            labels.append(label);
    }
    return labels;
}

QString visibleReasonText(QWidget *root, QWidget *control)
{
    QStringList texts;
    for (QLabel *label : visibleReasonLabels(root, control)) {
        const QString text = label->text().trimmed();
        if (!texts.contains(text))
            texts.append(text);
    }
    texts.sort();
    return texts.join(QStringLiteral(" | "));
}

// Visible labels that carry the control's DECLARED disabled reason (the
// tooltip/status tip/accessible description), i.e. evidence that the declared
// reason itself is presented as visible text rather than only as a tooltip.
QVector<QLabel *> visibleDeclaredReasonLabels(QWidget *root, QWidget *control)
{
    QVector<QLabel *> labels;
    const QString reason = declaredReason(control);
    if (reason.isEmpty())
        return labels;
    for (QLabel *label : root->findChildren<QLabel *>()) {
        const QString text = label->text().trimmed();
        if (text.isEmpty() || label->isHidden() || !label->isVisibleTo(root))
            continue;
        if (text.contains(reason) || (text.size() >= 4 && reason.contains(text)))
            labels.append(label);
    }
    return labels;
}

// A disabled control must present a reason that is visible on the page, not
// tooltip-only (brief OB-7). The reason may be declared in the tooltip/status
// tip/accessible description and rendered as a page label, or rendered as a
// non-empty label inside the control itself; either way a disabled control
// must not be explained only by a tooltip.
void requireVisibleDisabledReason(QWidget *root, QWidget *control, const char *name)
{
    QVERIFY2(!visibleReasonLabels(root, control).isEmpty(),
             qPrintable(QStringLiteral("the disabled %1 control shows no visible reason "
                                       "text (declared reason: '%2')")
                            .arg(QString::fromLatin1(name), declaredReason(control))));
}

// An enabled control must not still carry a disabled reason: neither the
// declared reason channel nor a visible label carrying that reason may remain.
void requireNoStaleDisabledReason(QWidget *root, QWidget *control, const char *name)
{
    QVERIFY2(declaredReason(control).isEmpty(),
             qPrintable(QStringLiteral("the enabled %1 control still declares a "
                                       "disabled reason: '%2'")
                            .arg(QString::fromLatin1(name), declaredReason(control))));
    QVERIFY2(visibleDeclaredReasonLabels(root, control).isEmpty(),
             qPrintable(QStringLiteral("the enabled %1 control still shows its old "
                                       "disabled reason: '%2'")
                            .arg(QString::fromLatin1(name),
                                 visibleReasonText(root, control))));
}

} // namespace

class CommandStatusProjectionTest : public QObject
{
    Q_OBJECT

private slots:
    // --- contract value: lifecycle classification ----------------------------
    void lifecycleStatesClassifyTerminalAndPending();
    void stringConversionIsStableAndNonEmpty();

    // --- ShellModel projection ----------------------------------------------
    void modelStoresStatusAndEmitsChange();
    void modelClearReturnsToIdle();

    // --- persistent, non-modal shell surface (OB-4) ---------------------------
    void actionBarLabelRendersLatestDetail();
    void latestDetailPersistsAcrossPageSwitches();
    void pendingAndTerminalStatesAreDistinguishable();
    void clearRemovesStaleTerminalText();
    void freshIdleHasNoStaleTerminalText();
    void actionBarRendersVisibleRejectionReason();

    // --- coordinator result is the sole adjust verdict (OB-9) -----------------
    void adjustVerdictRemainsCoordinatorFailureAfterSuccessLikeSnapshot();

    // --- PLC-HMI-011 OB-7: the manual-control surface shows the split ----------
    void manualSurfaceEnablesAllFourWhileHomeCompleteIsClear();
    void manualSurfaceEnablesAllFourOnceHomeCompleteIsSet();
};

void CommandStatusProjectionTest::lifecycleStatesClassifyTerminalAndPending()
{
    QVERIFY(!isTerminal(OperatorCommandState::Idle));
    QVERIFY(!isTerminal(OperatorCommandState::Rejected));
    QVERIFY(!isTerminal(OperatorCommandState::Accepted));
    QVERIFY(!isTerminal(OperatorCommandState::Pending));

    QVERIFY(isTerminal(OperatorCommandState::Succeeded));
    QVERIFY(isTerminal(OperatorCommandState::Failed));
    QVERIFY(isTerminal(OperatorCommandState::TimedOut));
    QVERIFY(isTerminal(OperatorCommandState::CommunicationsLost));
    QVERIFY(isTerminal(OperatorCommandState::GatewayReplaced));
}

void CommandStatusProjectionTest::stringConversionIsStableAndNonEmpty()
{
    const QVector<OperatorCommandState> states{
        OperatorCommandState::Idle,      OperatorCommandState::Rejected,
        OperatorCommandState::Accepted,  OperatorCommandState::Pending,
        OperatorCommandState::Succeeded, OperatorCommandState::Failed,
        OperatorCommandState::TimedOut,  OperatorCommandState::CommunicationsLost,
        OperatorCommandState::GatewayReplaced,
    };

    QSet<QString> rendered;
    for (OperatorCommandState state : states) {
        const QString text = toString(state);
        QVERIFY2(!text.isEmpty(), "every lifecycle state must render text");
        rendered.insert(text);
    }
    QCOMPARE(rendered.size(), states.size()); // distinct, stable identifiers
}

void CommandStatusProjectionTest::modelStoresStatusAndEmitsChange()
{
    ShellModel model;
    QVector<OperatorCommandStatus> changes;
    connect(&model, &ShellModel::operatorCommandStatusChanged, this,
            [&changes](const OperatorCommandStatus &s) { changes.append(s); });

    const OperatorCommandStatus pending =
        makeStatus(OperatorCommandState::Pending,
                   QStringLiteral("复位中: 等待切换到手动模式"));
    model.setOperatorCommandStatus(pending);

    QCOMPARE(changes.size(), 1);
    QVERIFY(changes[0].lifecycle_state == OperatorCommandState::Pending);
    QCOMPARE(changes[0].human_readable_detail, pending.human_readable_detail);

    const OperatorCommandStatus current = model.operatorCommandStatus();
    QVERIFY(current.lifecycle_state == OperatorCommandState::Pending);
    QCOMPARE(current.human_readable_detail, pending.human_readable_detail);

    const OperatorCommandStatus terminal =
        makeStatus(OperatorCommandState::Succeeded, QStringLiteral("复位完成"));
    model.setOperatorCommandStatus(terminal);
    QCOMPARE(changes.size(), 2);
    QVERIFY(model.operatorCommandStatus().lifecycle_state
            == OperatorCommandState::Succeeded);
    QCOMPARE(model.operatorCommandStatus().human_readable_detail,
             QStringLiteral("复位完成"));
}

void CommandStatusProjectionTest::modelClearReturnsToIdle()
{
    ShellModel model;
    model.setOperatorCommandStatus(
        makeStatus(OperatorCommandState::Failed, QStringLiteral("命令失败")));
    QCOMPARE(model.operatorCommandStatus().lifecycle_state,
             OperatorCommandState::Failed);

    model.clearOperatorCommandStatus();
    QVERIFY(model.operatorCommandStatus().lifecycle_state
            == OperatorCommandState::Idle);
    QVERIFY(!model.operatorCommandStatus().human_readable_detail.contains(
        QStringLiteral("命令失败")));
}

// --- persistent shell surface --------------------------------------------------

void CommandStatusProjectionTest::actionBarLabelRendersLatestDetail()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY2(label != nullptr, "ActionBar must expose a command status label");

    w.shellModel()->setOperatorCommandStatus(makeStatus(
        OperatorCommandState::Pending, QStringLiteral("等待 PLC 确认")));
    QApplication::processEvents();
    QVERIFY2(label->text().contains(QStringLiteral("等待 PLC 确认")),
             qPrintable(QStringLiteral("pending detail not rendered, label=%1")
                            .arg(label->text())));

    w.shellModel()->setOperatorCommandStatus(makeStatus(
        OperatorCommandState::TimedOut,
        QStringLiteral("等待超时, 请检查设备"), Command::Start));
    QApplication::processEvents();
    QVERIFY2(label->text().contains(QStringLiteral("等待超时, 请检查设备")),
             qPrintable(QStringLiteral("terminal detail not rendered, label=%1")
                            .arg(label->text())));

    // Non-modal: the label lives on the shell and does not open a dialog.
    QVERIFY(label->isVisible());
    QVERIFY(!QApplication::activeModalWidget());
}

void CommandStatusProjectionTest::latestDetailPersistsAcrossPageSwitches()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY(label != nullptr);

    const QString detail = QStringLiteral("调宽完成");
    w.shellModel()->setOperatorCommandStatus(
        makeStatus(OperatorCommandState::Succeeded, detail, Command::AdjustWidth));
    QVERIFY(label->text().contains(detail));

    for (int page = 0; page < 7; ++page) {
        w.setCurrentPage(page);
        QApplication::processEvents();
        QVERIFY2(label->text().contains(detail),
                 qPrintable(QStringLiteral("terminal detail lost on page %1")
                                .arg(page)));
    }
}

void CommandStatusProjectionTest::pendingAndTerminalStatesAreDistinguishable()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY(label != nullptr);

    w.shellModel()->setOperatorCommandStatus(
        makeStatus(OperatorCommandState::Pending, QStringLiteral("等待确认")));
    const QString pendingText = label->text();
    const OperatorCommandState pendingState =
        w.shellModel()->operatorCommandStatus().lifecycle_state;

    w.shellModel()->setOperatorCommandStatus(makeStatus(
        OperatorCommandState::Succeeded, QStringLiteral("执行成功"), Command::Start));
    const QString terminalText = label->text();
    const OperatorCommandState terminalState =
        w.shellModel()->operatorCommandStatus().lifecycle_state;

    QVERIFY(pendingText.contains(QStringLiteral("等待确认")));
    QVERIFY(terminalText.contains(QStringLiteral("执行成功")));
    QVERIFY2(pendingText != terminalText,
             "pending and terminal states must render differently");
    QVERIFY(!isTerminal(pendingState));
    QVERIFY(isTerminal(terminalState));
}

void CommandStatusProjectionTest::clearRemovesStaleTerminalText()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY(label != nullptr);

    const QString stale = QStringLiteral("调宽等待超时, 请检查设备");
    w.shellModel()->setOperatorCommandStatus(
        makeStatus(OperatorCommandState::TimedOut, stale, Command::AdjustWidth));
    QVERIFY(label->text().contains(stale));

    w.shellModel()->clearOperatorCommandStatus();
    QApplication::processEvents();
    QVERIFY(w.shellModel()->operatorCommandStatus().lifecycle_state
            == OperatorCommandState::Idle);
    QVERIFY2(!label->text().contains(stale),
             "cleared status must not keep rendering stale terminal text");
}

void CommandStatusProjectionTest::freshIdleHasNoStaleTerminalText()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY(label != nullptr);

    QVERIFY(w.shellModel()->operatorCommandStatus().lifecycle_state
            == OperatorCommandState::Idle);
    QVERIFY2(!label->text().contains(QStringLiteral("成功")),
             "idle status must not display success text");
    QVERIFY2(!label->text().contains(QStringLiteral("失败")),
             "idle status must not display failure text");
}

void CommandStatusProjectionTest::actionBarRendersVisibleRejectionReason()
{
    // Brief OB-1/OB-4/OB-11: a rejection is an operator-visible state whose
    // non-empty human-readable reason must remain visible on the shell surface.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY(label != nullptr);

    const QString reason = QStringLiteral("权限不足: 需要管理员");
    w.shellModel()->setOperatorCommandStatus(
        makeStatus(OperatorCommandState::Rejected, reason, Command::Reset));
    QApplication::processEvents();

    QVERIFY2(label->text().contains(reason),
             qPrintable(QStringLiteral("rejection reason not rendered, label=%1")
                            .arg(label->text())));
    QVERIFY(!QApplication::activeModalWidget());
}

// --- OB-9: coordinator result is the sole adjust verdict -----------------------

void CommandStatusProjectionTest::adjustVerdictRemainsCoordinatorFailureAfterSuccessLikeSnapshot()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QLabel *label = bar->commandStatusLabel();
    QVERIFY(label != nullptr);

    // The coordinator already reported a terminal adjust failure/timeout.
    const QString failureDetail = QStringLiteral("调宽等待超时, 请检查设备");
    w.shellModel()->setOperatorCommandStatus(
        makeStatus(OperatorCommandState::TimedOut, failureDetail,
                   Command::AdjustWidth));

    // Later snapshots contain success-like bits (M44=1, D130 == target).
    DeviceSnapshotData successLike = validSnapshotData();
    successLike.statusWord3 = (quint16(1) << 14); // M44
    successLike.currentWidth = 300;               // D130 equals the target
    successLike.targetWidth = 300;                // D128
    w.shellModel()->updateSnapshot(DeviceSnapshot(successLike));
    QApplication::processEvents();

    // A coordinator failure/timeout must not be displayed as success.
    QVERIFY(w.shellModel()->operatorCommandStatus().lifecycle_state
            == OperatorCommandState::TimedOut);
    QVERIFY(w.shellModel()->operatorCommandStatus().human_readable_detail.contains(
        failureDetail));
    QVERIFY2(!label->text().contains(QStringLiteral("调宽成功")),
             "success-like snapshot must not override the coordinator verdict");
}

// --- PLC-HMI-011 OB-7: the operator console shows the split --------------------

void CommandStatusProjectionTest::manualSurfaceEnablesAllFourWhileHomeCompleteIsClear()
{
    // Brief OB-7 / user decision 2026-09-21: with home-complete CLEAR and the
    // other common conditions satisfied (administrator, online, manual mode,
    // not running, no emergency stop, no latched fault), ALL FOUR manual
    // controls are enabled — homing completion is no longer an HMI manual gate
    // — and no control shows a stale disabled reason. The 回原点 control is
    // what the operator uses to home the machine.
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ManualControlPage page(model);
    model.updateSnapshot(
        DeviceSnapshot(manualSurfaceData(/*homeComplete=*/false)));
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    QVERIFY2(page.jogButton() != nullptr, "the manual page must expose the belt-jog control");
    QVERIFY2(page.widthFwdButton() != nullptr,
             "the manual page must expose the width-forward control");
    QVERIFY2(page.widthRevButton() != nullptr,
             "the manual page must expose the width-reverse control");
    QVERIFY2(page.stopGateButton() != nullptr,
             "the manual page must expose the stop-gate control");

    // Every manual control is enabled while home-complete is clear.
    QVERIFY2(page.jogButton()->isEnabled(),
             qPrintable(QStringLiteral("the belt-jog control must be enabled while "
                                       "home-complete is clear (reason: '%1')")
                            .arg(declaredReason(page.jogButton()))));
    QVERIFY2(page.stopGateButton()->isEnabled(),
             qPrintable(QStringLiteral("the stop-gate control must be enabled while "
                                       "home-complete is clear (reason: '%1')")
                            .arg(declaredReason(page.stopGateButton()))));
    QVERIFY2(page.widthFwdButton()->isEnabled(),
             qPrintable(QStringLiteral("the width-forward control must be enabled while "
                                       "home-complete is clear (reason: '%1')")
                            .arg(declaredReason(page.widthFwdButton()))));
    QVERIFY2(page.widthRevButton()->isEnabled(),
             qPrintable(QStringLiteral("the width-reverse control must be enabled while "
                                       "home-complete is clear (reason: '%1')")
                            .arg(declaredReason(page.widthRevButton()))));

    // No enabled control may keep a stale disabled reason visible.
    requireNoStaleDisabledReason(&page, page.jogButton(), "belt-jog");
    requireNoStaleDisabledReason(&page, page.stopGateButton(), "stop-gate");
    requireNoStaleDisabledReason(&page, page.widthFwdButton(), "width-forward");
    requireNoStaleDisabledReason(&page, page.widthRevButton(), "width-reverse");
}

void CommandStatusProjectionTest::manualSurfaceEnablesAllFourOnceHomeCompleteIsSet()
{
    // Brief OB-7: once home-complete is set (and the other common conditions
    // still hold) all four manual controls are enabled and no disabled reason
    // remains visible.
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ManualControlPage page(model);
    model.updateSnapshot(
        DeviceSnapshot(manualSurfaceData(/*homeComplete=*/true)));
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    QVERIFY2(page.jogButton() != nullptr, "the manual page must expose the belt-jog control");
    QVERIFY2(page.widthFwdButton() != nullptr,
             "the manual page must expose the width-forward control");
    QVERIFY2(page.widthRevButton() != nullptr,
             "the manual page must expose the width-reverse control");
    QVERIFY2(page.stopGateButton() != nullptr,
             "the manual page must expose the stop-gate control");

    QVERIFY2(page.jogButton()->isEnabled(),
             qPrintable(QStringLiteral("the belt-jog control must be enabled once "
                                       "home-complete is set (reason: '%1')")
                            .arg(declaredReason(page.jogButton()))));
    QVERIFY2(page.widthFwdButton()->isEnabled(),
             qPrintable(QStringLiteral("the width-forward control must be enabled once "
                                       "home-complete is set (reason: '%1')")
                            .arg(declaredReason(page.widthFwdButton()))));
    QVERIFY2(page.widthRevButton()->isEnabled(),
             qPrintable(QStringLiteral("the width-reverse control must be enabled once "
                                       "home-complete is set (reason: '%1')")
                            .arg(declaredReason(page.widthRevButton()))));
    QVERIFY2(page.stopGateButton()->isEnabled(),
             qPrintable(QStringLiteral("the stop-gate control must be enabled once "
                                       "home-complete is set (reason: '%1')")
                            .arg(declaredReason(page.stopGateButton()))));

    // No enabled control may still present a disabled reason.
    QWidget *const enabledControls[] = {
        page.jogButton(), page.widthFwdButton(), page.widthRevButton(),
        page.stopGateButton()};
    const char *const enabledNames[] = {"belt-jog", "width-forward",
                                        "width-reverse", "stop-gate"};
    for (int i = 0; i < 4; ++i) {
        requireNoStaleDisabledReason(&page, enabledControls[i], enabledNames[i]);
    }
}

QTEST_MAIN(CommandStatusProjectionTest)
#include "command_status_projection_test.moc"
