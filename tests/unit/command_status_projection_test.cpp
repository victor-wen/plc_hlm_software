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

#include <QtTest>

#include <QLabel>
#include <QSet>
#include <QVector>

#include "domain/device_snapshot.h"
#include "domain/operator_command_status.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
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

QTEST_MAIN(CommandStatusProjectionTest)
#include "command_status_projection_test.moc"
