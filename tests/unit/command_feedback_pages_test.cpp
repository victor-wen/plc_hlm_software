// PLC-HMI-001 black-box tests: page-local command feedback for the settings
// D204 verify/write flow (brief OB-8) and the recipe/width adjust verdict
// (brief OB-9).
//
// These tests use the existing public page/model APIs, so their expected RED is
// a runtime assertion failure. The pure contract-API projection is covered in
// command_status_projection_test.cpp.

#include <QtTest>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/dialogs/admin_password_dialog.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210
    d.pulsePerMm = 1280;   // D204
    d.widthSpeed = 15;     // D220
    d.beltSpeed = 1000;    // D122
    d.heartbeat = 1;       // D140
    d.fastQuality = DataQuality::Valid;
    d.homeQuality = DataQuality::Valid;
    d.commandQuality = DataQuality::Valid;
    d.slowQuality = DataQuality::Valid;
    d.overallQuality = aggregateQuality(d);
    return d;
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

class CommandFeedbackPagesTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-8: settings D204 verify/write pending + terminal ------------------
    void d204WriteFlowShowsPendingBeforeTerminalResult();

    // --- OB-9: adjust verdict is not re-derived from snapshots ----------------
    void adjustFailureNotOverriddenBySuccessLikeSnapshot();
    void adjustSuccessNotOverriddenByFailureLikeSnapshot();
};

// --- OB-8 ---------------------------------------------------------------------

void CommandFeedbackPagesTest::d204WriteFlowShowsPendingBeforeTerminalResult()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    QSignalSpy writeSpy(&page, &UsersSettingsPage::d204WriteRequested);

    page.d204Spin()->setValue(2560);
    clickAt(page.writeD204Button());

    auto *dialog = page.findChild<AdminPasswordDialog *>();
    QVERIFY(dialog != nullptr);
    dialog->passwordEdit()->setText(QStringLiteral("admin-secret"));
    clickAt(dialog->okButton());
    QCOMPARE(writeSpy.count(), 1); // verification + write request dispatched

    // Pending state must be visible before the terminal result: "验证" or
    // "写入中" (brief OB-8).
    const QString pendingText = page.paramStatusText();
    QVERIFY2(pendingText.contains(QStringLiteral("验证"))
                 || pendingText.contains(QStringLiteral("写入中")),
             qPrintable(QStringLiteral("no pending D204 feedback, status=%1")
                            .arg(pendingText)));
    QVERIFY2(!pendingText.contains(QStringLiteral("成功")),
             "pending D204 feedback must not claim success");

    // The terminal result remains visible afterwards.
    page.setParameterWriteResult(true, QStringLiteral("写入成功"));
    QVERIFY(page.paramStatusText().contains(QStringLiteral("成功")));

    page.setParameterWriteResult(false, QStringLiteral("写入失败, 请重试"));
    QVERIFY(page.paramStatusText().contains(QStringLiteral("失败")));
}

// --- OB-9 ---------------------------------------------------------------------

void CommandFeedbackPagesTest::adjustFailureNotOverriddenBySuccessLikeSnapshot()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // Dispatch an apply for 300 (two-step confirm on the page).
    page.widthSpin()->setValue(300);
    clickAt(page.applyButton());
    clickAt(page.applyButton());
    QVERIFY(page.statusText().contains(QStringLiteral("等待 PLC 结果")));

    // The coordinator's most recent terminal result is a failure/timeout.
    page.setAdjustResult(false, QStringLiteral("调宽等待超时, 请检查设备"));
    QVERIFY(page.statusText().contains(QStringLiteral("超时")));
    QVERIFY(!page.statusText().contains(QStringLiteral("调宽成功")));

    // A later snapshot contains success-like bits (M44=1, D130 == target).
    DeviceSnapshotData successLike = validSnapshotData();
    successLike.statusWord3 = (quint16(1) << 14); // M44
    successLike.currentWidth = 300;
    successLike.targetWidth = 300;
    model.updateSnapshot(DeviceSnapshot(successLike));
    QApplication::processEvents();

    QVERIFY2(!page.statusText().contains(QStringLiteral("调宽成功")),
             qPrintable(QStringLiteral("success-like snapshot overrode the "
                                       "coordinator failure: %1")
                            .arg(page.statusText())));
    QVERIFY(page.statusText().contains(QStringLiteral("超时")));
}

void CommandFeedbackPagesTest::adjustSuccessNotOverriddenByFailureLikeSnapshot()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    page.widthSpin()->setValue(300);
    clickAt(page.applyButton());
    clickAt(page.applyButton());
    QVERIFY(page.statusText().contains(QStringLiteral("等待 PLC 结果")));

    // The coordinator's most recent terminal result is success.
    page.setAdjustResult(true, QStringLiteral("调宽完成"));
    QVERIFY(page.statusText().contains(QStringLiteral("调宽完成")));

    // A later snapshot contains failure-like bits (M45=1, M34=0).
    DeviceSnapshotData failureLike = validSnapshotData();
    failureLike.statusWord3 = (quint16(1) << 15); // M45
    model.updateSnapshot(DeviceSnapshot(failureLike));
    QApplication::processEvents();

    QVERIFY2(!page.statusText().contains(QStringLiteral("调宽失败")),
             qPrintable(QStringLiteral("failure-like snapshot overrode the "
                                       "coordinator success: %1")
                            .arg(page.statusText())));
    QVERIFY(page.statusText().contains(QStringLiteral("调宽完成")));
}

QTEST_MAIN(CommandFeedbackPagesTest)
#include "command_feedback_pages_test.moc"
