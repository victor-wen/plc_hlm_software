// PLC-HMI-008 black-box unit tests: a control disabled because a permission,
// an interlock or communication is unmet must present its exact reason as
// visible inline text -- not only as a tooltip/status tip. Distinct blocking
// causes must be distinguishable in the visible text, the reason must track
// allowed<->disabled transitions, displaying a reason must never make a
// disabled control clickable, and allowed controls (Stop / software estop)
// must keep their existing enabling semantics (brief OB-5, OB-6, OB-7, OB-8;
// contract OperatorCommandStatus.presentation.disabled_reasons and the
// invariant "disabled widgets never rely on tooltip-only explanation").
//
// Authored only from .ai/test-briefs/PLC-HMI-008.yaml, the approved
// .ai/project-contract.yaml and inspectable test sources under tests/**. No
// production implementation source was read.
//
// This file deliberately uses only existing page/model APIs plus generic Qt
// introspection (QAbstractButton / QLabel / toolTip / statusTip / accessible
// description / widget geometry), so the expected RED is an observable runtime
// assertion failure: today a disabled control's declared reason is
// tooltip-only and no visible label carries it.

#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QStringList>
#include <QVector>

#include "domain/device_snapshot.h"
#include "ui/MainWindow.h"
#include "ui/pages/manual_control_page.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

// The contract fixes the requirement (visible, adjacent, touch-readable
// reason) but no exact wording, widget class or pixel size. The two constants
// below are frozen author assumptions recorded in the author-phase RED report;
// they are deliberately generous so a legitimate implementation cannot fail
// them accidentally.
constexpr int kAdjacencyPx = 300;
constexpr int kMinimumLegibleFontHeight = 12;

// Machine data matching the existing page-test fixtures: M1 manual / M2
// automatic, M9 homed (M61 per COMMMAP), valid quality blocks.
DeviceSnapshotData snapshotData(bool homed, bool automatic, bool latchedFault = false)
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
    if (latchedFault)
        d.statusWord1 |= quint16(1) << 14; // M14 latched fault
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 1280;
    d.widthSpeed = 15;
    d.beltSpeed = 1000;
    d.heartbeat = 1;
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
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

// The reason a control declares for being disabled, independent of any
// production API: tooltip, status tip or accessible description.
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

// Text labels that would actually be readable on the presented page: not
// hidden, not inside a hidden container, non-empty text.
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

QVector<QAbstractButton *> disabledControlsWithDeclaredReason(QWidget *root)
{
    QVector<QAbstractButton *> controls;
    for (QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
        if (!button->isEnabled() && !declaredReason(button).isEmpty())
            controls.append(button);
    }
    return controls;
}

bool isNear(QWidget *root, const QWidget *control, const QWidget *other)
{
    const QPoint a = control->mapTo(root, control->rect().center());
    const QPoint b = other->mapTo(root, other->rect().center());
    return (a - b).manhattanLength() <= kAdjacencyPx;
}

// Visible labels that carry the control's declared reason. A label whose whole
// text is contained in the declared reason (>= 4 characters) also counts, so
// the implementation may shorten the wording without losing the semantics.
QVector<QLabel *> matchingReasonLabels(QWidget *root, QWidget *control)
{
    const QString reason = declaredReason(control);
    QVector<QLabel *> matches;
    if (reason.isEmpty())
        return matches;
    for (QLabel *label : visibleTextLabels(root)) {
        const QString text = label->text().trimmed();
        if (text.contains(reason) || (text.size() >= 4 && reason.contains(text)))
            matches.append(label);
    }
    return matches;
}

// The union of visible reason texts of every disabled control that declares a
// reason on the given page.
QStringList visibleReasonTexts(QWidget *root)
{
    QStringList texts;
    for (QAbstractButton *button : disabledControlsWithDeclaredReason(root)) {
        for (QLabel *label : matchingReasonLabels(root, button)) {
            const QString text = label->text().trimmed();
            if (!texts.contains(text))
                texts.append(text);
        }
    }
    texts.sort();
    return texts;
}

void requireVisibleReasonForEveryDisabledControl(QWidget *root, const char *page)
{
    const QVector<QAbstractButton *> controls = disabledControlsWithDeclaredReason(root);
    QVERIFY2(!controls.isEmpty(),
             qPrintable(QStringLiteral(
                            "%1: precondition failed -- no disabled control with a "
                            "declared reason was found for the exercised state")
                            .arg(QString::fromLatin1(page))));
    for (QAbstractButton *control : controls) {
        QVERIFY2(!matchingReasonLabels(root, control).isEmpty(),
                 qPrintable(QStringLiteral(
                                "%1: disabled control '%2' declares the reason '%3' "
                                "but no visible label carries it (tooltip-only "
                                "explanation)")
                                .arg(QString::fromLatin1(page),
                                     control->text().trimmed(),
                                     declaredReason(control))));
    }
}

} // namespace

class VisibleDisabledReasonsTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-5: visible reason text while disabled, per production surface -----
    void actionBarDisabledControlsExposeVisibleReasonText();
    void manualControlDisabledControlsExposeVisibleReasonText();
    void recipePageDisabledControlsExposeVisibleReasonText();
    void visibleReasonsSitAdjacentToTheirDisabledControls();
    void visibleReasonTextsRemainLegibleAtTouchSizes();

    // --- OB-6: distinct causes produce distinguishable visible text -----------
    void distinctBlockingCausesProduceDistinctVisibleReasons();
    void actionBarReasonsFollowPermissionAndCommunicationStates();

    // --- OB-8: transitions allowed->disabled and disabled->allowed ------------
    void recipeReasonAppearsAndDisappearsAcrossRoleTransitions();
    void sameSurfaceReasonTransitionsDistinguishCommunicationAndInterlock();

    // --- OB-7: enabling semantics unchanged -----------------------------------
    void disabledControlsStayUnclickableWhileAReasonIsVisible();
};

// --- OB-5 ----------------------------------------------------------------------

void VisibleDisabledReasonsTest::actionBarDisabledControlsExposeVisibleReasonText()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    w.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    requireVisibleReasonForEveryDisabledControl(bar, "action bar, anonymous");
}

void VisibleDisabledReasonsTest::manualControlDisabledControlsExposeVisibleReasonText()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QString(), Role::Anonymous);
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    requireVisibleReasonForEveryDisabledControl(&page, "manual control, anonymous");
}

void VisibleDisabledReasonsTest::recipePageDisabledControlsExposeVisibleReasonText()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QString(), Role::Anonymous);
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    requireVisibleReasonForEveryDisabledControl(&page, "recipe page, anonymous");
}

void VisibleDisabledReasonsTest::visibleReasonsSitAdjacentToTheirDisabledControls()
{
    // OB-5: the visible reason text is adjacent to the control, not merely
    // present somewhere on the application.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    w.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();

    ShellModel manualModel;
    ManualControlPage manual(manualModel);
    manualModel.setUser(QString(), Role::Anonymous);
    manual.resize(1280, 720);
    manual.show();

    ShellModel recipeModel;
    RecipeWidthPage recipe(recipeModel);
    recipeModel.setUser(QString(), Role::Anonymous);
    recipe.resize(1280, 720);
    recipe.show();
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    const struct
    {
        QWidget *root;
        const char *page;
    } surfaces[] = {{bar, "action bar"}, {&manual, "manual control"}, {&recipe, "recipe page"}};

    for (const auto &surface : surfaces) {
        const QVector<QAbstractButton *> controls =
            disabledControlsWithDeclaredReason(surface.root);
        QVERIFY2(!controls.isEmpty(),
                 qPrintable(QStringLiteral("%1: no disabled control with a declared "
                                           "reason was found")
                                .arg(QString::fromLatin1(surface.page))));
        for (QAbstractButton *control : controls) {
            const QVector<QLabel *> labels =
                matchingReasonLabels(surface.root, control);
            QVERIFY2(!labels.isEmpty(),
                     qPrintable(QStringLiteral("%1: disabled control '%2' has no "
                                               "visible reason label")
                                    .arg(QString::fromLatin1(surface.page),
                                         control->text().trimmed())));
            bool adjacent = false;
            for (QLabel *label : labels) {
                if (isNear(surface.root, control, label)) {
                    adjacent = true;
                    break;
                }
            }
            QVERIFY2(adjacent,
                     qPrintable(QStringLiteral("%1: the visible reason for '%2' is "
                                               "not adjacent to the control")
                                    .arg(QString::fromLatin1(surface.page),
                                         control->text().trimmed())));
        }
    }
}

void VisibleDisabledReasonsTest::visibleReasonTextsRemainLegibleAtTouchSizes()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    w.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();
    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    ShellModel manualModel;
    ManualControlPage manual(manualModel);
    manualModel.setUser(QString(), Role::Anonymous);
    manual.resize(1280, 720);
    manual.show();
    QApplication::processEvents();

    QWidget *const surfaces[] = {bar, &manual};
    for (QWidget *root : surfaces) {
        const QVector<QAbstractButton *> controls =
            disabledControlsWithDeclaredReason(root);
        QVERIFY2(!controls.isEmpty(),
                 "precondition: a disabled control with a declared reason must exist");
        for (QAbstractButton *control : controls) {
            const QVector<QLabel *> labels = matchingReasonLabels(root, control);
            QVERIFY2(!labels.isEmpty(),
                     "a disabled control must show its reason as visible text");
            for (QLabel *label : labels) {
                QVERIFY2(!label->isHidden(), "the reason text must not be hidden");
                QVERIFY2(label->fontMetrics().height() >= kMinimumLegibleFontHeight,
                         qPrintable(QStringLiteral(
                                        "reason text font height %1 is below the "
                                        "legible minimum %2")
                                        .arg(label->fontMetrics().height())
                                        .arg(kMinimumLegibleFontHeight)));
            }
        }
    }
}

// --- OB-6 ----------------------------------------------------------------------

void VisibleDisabledReasonsTest::distinctBlockingCausesProduceDistinctVisibleReasons()
{
    // Permission (anonymous + valid data), communication (administrator, no
    // snapshot) and interlock (administrator + connected but not homed) must be
    // distinguishable in the visible reason text, on both the recipe apply
    // surface and the manual control surface.
    ShellModel recipePermissionModel;
    RecipeWidthPage recipePermission(recipePermissionModel);
    recipePermissionModel.setUser(QString(), Role::Anonymous);
    recipePermissionModel.updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    recipePermission.show();

    ShellModel recipeCommModel;
    RecipeWidthPage recipeComm(recipeCommModel);
    recipeCommModel.setUser(QStringLiteral("admin"), Role::Admin);
    recipeComm.show();

    ShellModel recipeInterlockModel;
    RecipeWidthPage recipeInterlock(recipeInterlockModel);
    recipeInterlockModel.setUser(QStringLiteral("admin"), Role::Admin);
    // User decision 2026-09-21: an un-homed machine is no longer an interlock
    // cause; a latched fault (M14) still blocks both surfaces.
    recipeInterlockModel.updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false,
                                    /*latchedFault=*/true)));
    recipeInterlock.show();

    ShellModel manualPermissionModel;
    ManualControlPage manualPermission(manualPermissionModel);
    manualPermissionModel.setUser(QString(), Role::Anonymous);
    manualPermissionModel.updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    manualPermission.show();

    ShellModel manualCommModel;
    ManualControlPage manualComm(manualCommModel);
    manualCommModel.setUser(QStringLiteral("admin"), Role::Admin);
    manualComm.show();

    ShellModel manualInterlockModel;
    ManualControlPage manualInterlock(manualInterlockModel);
    manualInterlockModel.setUser(QStringLiteral("admin"), Role::Admin);
    manualInterlockModel.updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false,
                                    /*latchedFault=*/true)));
    manualInterlock.show();

    QApplication::processEvents();

    const QStringList recipePermissionTexts = visibleReasonTexts(&recipePermission);
    const QStringList recipeCommTexts = visibleReasonTexts(&recipeComm);
    const QStringList recipeInterlockTexts = visibleReasonTexts(&recipeInterlock);
    const QStringList manualPermissionTexts = visibleReasonTexts(&manualPermission);
    const QStringList manualCommTexts = visibleReasonTexts(&manualComm);
    const QStringList manualInterlockTexts = visibleReasonTexts(&manualInterlock);

    QVERIFY2(!recipePermissionTexts.isEmpty(),
             "no visible reason for the disabled recipe control under a permission block");
    QVERIFY2(!recipeCommTexts.isEmpty(),
             "no visible reason for the disabled recipe control under a communication block");
    QVERIFY2(!recipeInterlockTexts.isEmpty(),
             "no visible reason for the disabled recipe control under an interlock block");
    QVERIFY2(!manualPermissionTexts.isEmpty(),
             "no visible reason for a disabled manual control under a permission block");
    QVERIFY2(!manualCommTexts.isEmpty(),
             "no visible reason for a disabled manual control under a communication block");
    QVERIFY2(!manualInterlockTexts.isEmpty(),
             "no visible reason for a disabled manual control under an interlock block");

    QVERIFY2(recipePermissionTexts != recipeCommTexts,
             "permission and communication blocks are not distinguishable on the recipe page");
    QVERIFY2(recipePermissionTexts != recipeInterlockTexts,
             "permission and interlock blocks are not distinguishable on the recipe page");
    QVERIFY2(recipeCommTexts != recipeInterlockTexts,
             "communication and interlock blocks are not distinguishable on the recipe page");
    QVERIFY2(manualPermissionTexts != manualCommTexts,
             "permission and communication blocks are not distinguishable on the manual page");
    QVERIFY2(manualPermissionTexts != manualInterlockTexts,
             "permission and interlock blocks are not distinguishable on the manual page");
    QVERIFY2(manualCommTexts != manualInterlockTexts,
             "communication and interlock blocks are not distinguishable on the manual page");
}

void VisibleDisabledReasonsTest::actionBarReasonsFollowPermissionAndCommunicationStates()
{
    MainWindow permissionWindow;
    permissionWindow.resize(1920, 1080);
    permissionWindow.show();
    QApplication::processEvents();
    permissionWindow.shellModel()->setUser(QString(), Role::Anonymous);
    permissionWindow.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QApplication::processEvents();
    ActionBar *permissionBar = permissionWindow.findChild<ActionBar *>();
    QVERIFY(permissionBar != nullptr);

    MainWindow commWindow;
    commWindow.resize(1920, 1080);
    commWindow.show();
    QApplication::processEvents();
    commWindow.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    QApplication::processEvents();
    ActionBar *commBar = commWindow.findChild<ActionBar *>();
    QVERIFY(commBar != nullptr);

    const QStringList permissionTexts = visibleReasonTexts(permissionBar);
    const QStringList commTexts = visibleReasonTexts(commBar);

    QVERIFY2(!permissionTexts.isEmpty(),
             "no visible reason for a disabled action-bar control under a permission block");
    QVERIFY2(!commTexts.isEmpty(),
             "no visible reason for a disabled action-bar control under a communication block");
    QVERIFY2(permissionTexts != commTexts,
             "permission and communication blocks are not distinguishable on the action bar");
}

// --- OB-8 ----------------------------------------------------------------------

void VisibleDisabledReasonsTest::recipeReasonAppearsAndDisappearsAcrossRoleTransitions()
{
    ShellModel model;
    model.setUser(QString(), Role::Anonymous);
    RecipeWidthPage page(model);
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    QVERIFY2(!page.applyButton()->isEnabled(),
             "precondition: an anonymous session must disable the recipe apply control");
    const QStringList disabledTexts = visibleReasonTexts(&page);
    QVERIFY2(!disabledTexts.isEmpty(),
             "the disabled recipe apply control shows no visible reason");

    // disabled -> allowed: administrator with connected, homed, manual data.
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QTRY_VERIFY_WITH_TIMEOUT(page.applyButton()->isEnabled(), 2000);
    const QStringList allowedTexts = visibleReasonTexts(&page);
    for (const QString &text : disabledTexts) {
        QVERIFY2(!allowedTexts.contains(text),
                 qPrintable(QStringLiteral("a stale disabled reason survived the "
                                           "transition to an allowed state: '%1'")
                                .arg(text)));
    }

    // allowed -> disabled: back to anonymous.
    model.setUser(QString(), Role::Anonymous);
    QTRY_VERIFY_WITH_TIMEOUT(!page.applyButton()->isEnabled(), 2000);
    QVERIFY2(!visibleReasonTexts(&page).isEmpty(),
             "the recipe apply control lost its visible reason after the transition "
             "back to disabled");
}

void VisibleDisabledReasonsTest::sameSurfaceReasonTransitionsDistinguishCommunicationAndInterlock()
{
    // OB-5/OB-6/OB-8 on one production surface: while the same control is
    // blocked the visible reason must track the current cause, must not
    // survive into the allowed state, and must reappear distinguishably when
    // a different cause blocks the control again.
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    // Cause 1: communication block -- administrator session, no snapshot.
    QVERIFY2(!page.applyButton()->isEnabled(),
             "precondition: the recipe apply control must be disabled under a "
             "communication block");
    const QString commReason = declaredReason(page.applyButton());
    QVERIFY2(!commReason.isEmpty(),
             "precondition: the disabled recipe apply control must declare a reason");
    const QVector<QLabel *> commLabels = matchingReasonLabels(&page, page.applyButton());
    QVERIFY2(!commLabels.isEmpty(),
             "the communication-blocked recipe apply control shows no visible reason");
    QStringList commTexts;
    QVector<QPointer<QLabel>> commLabelRefs;
    for (QLabel *label : commLabels) {
        commTexts.append(label->text().trimmed());
        commLabelRefs.append(QPointer<QLabel>(label));
    }
    commTexts.sort();

    // disabled -> allowed: administrator session with connected, homed, manual
    // data (the same state the sibling transition case uses).
    model.updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QTRY_VERIFY_WITH_TIMEOUT(page.applyButton()->isEnabled(), 2000);

    // The previously visible reason must not survive the allowed transition.
    for (const QPointer<QLabel> &label : commLabelRefs) {
        if (label.isNull())
            continue;
        const QString text = label->text().trimmed();
        const bool carries = text.contains(commReason) ||
                             (text.size() >= 4 && commReason.contains(text));
        QVERIFY2(label->isHidden() || !label->isVisibleTo(&page) || !carries,
                 qPrintable(QStringLiteral(
                                "a visible label still carries the previous "
                                "communication reason '%1' after the control became "
                                "allowed")
                                .arg(commReason)));
    }

    // allowed -> disabled with a different cause: interlock (latched fault,
    // user decision 2026-09-21: homing is no longer an HMI adjust gate).
    model.updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false,
                                    /*latchedFault=*/true)));
    QTRY_VERIFY_WITH_TIMEOUT(!page.applyButton()->isEnabled(), 2000);
    const QVector<QLabel *> interlockLabels =
        matchingReasonLabels(&page, page.applyButton());
    QVERIFY2(!interlockLabels.isEmpty(),
             "the interlock-blocked recipe apply control shows no visible reason");
    QStringList interlockTexts;
    for (QLabel *label : interlockLabels)
        interlockTexts.append(label->text().trimmed());
    interlockTexts.sort();
    QVERIFY2(interlockTexts != commTexts,
             "the visible reason did not change distinguishably when the blocking "
             "cause changed from communication to interlock");
}

// --- OB-7 ----------------------------------------------------------------------

void VisibleDisabledReasonsTest::disabledControlsStayUnclickableWhileAReasonIsVisible()
{
    // The recipe surface: clicking a disabled control must produce no request.
    ShellModel recipeModel;
    recipeModel.setUser(QString(), Role::Anonymous);
    RecipeWidthPage recipe(recipeModel);
    recipe.resize(1280, 720);
    recipe.show();
    QApplication::processEvents();

    QSignalSpy applySpy(&recipe, &RecipeWidthPage::applyAdjustRequested);
    QSignalSpy saveSpy(&recipe, &RecipeWidthPage::saveRecipeRequested);
    QSignalSpy deleteSpy(&recipe, &RecipeWidthPage::deleteRecipeRequested);
    clickAt(recipe.applyButton());
    clickAt(recipe.saveButton());
    clickAt(recipe.deleteButton());
    QCOMPARE(applySpy.count(), 0);
    QCOMPARE(saveSpy.count(), 0);
    QCOMPARE(deleteSpy.count(), 0);
    QVERIFY(!recipe.applyButton()->isEnabled());

    // The action bar: clicking every disabled, reason-carrying control must
    // produce no command request, while at least one control (Stop / software
    // estop) stays enabled and usable.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    w.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();
    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    QSignalSpy actionSpy(bar, &ActionBar::actionRequested);
    for (QAbstractButton *control : disabledControlsWithDeclaredReason(bar)) {
        clickAt(control);
        QVERIFY2(!control->isEnabled(), "a clicked disabled control must stay disabled");
    }
    QCOMPARE(actionSpy.count(), 0);

    // Premise correction (2026-09-16): the enabling-semantics assertion below is
    // about an ONLINE anonymous session. Spec §11.4 grants anonymous online
    // Stop and online software-estop set; spec §10.5/§10.6 require an offline
    // session to refuse to send those commands and point at the physical
    // stop/estop instead. A fresh shell is offline, so zero enabled action-bar
    // controls is the correct offline behavior and cannot be the premise of
    // this assertion. Feed the same valid online snapshot the sibling cases use
    // through the production page/shell-model API (ShellModel::updateSnapshot)
    // so at least one usable control (online Stop and/or software estop) must
    // remain enabled. The click assertions above stay in the offline state and
    // are unaffected.
    w.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QApplication::processEvents();

    int enabledControls = 0;
    for (QAbstractButton *button : bar->findChildren<QAbstractButton *>()) {
        if (button->isEnabled() && button->isVisibleTo(bar))
            ++enabledControls;
    }
    QVERIFY2(enabledControls > 0,
             "at least one action-bar control must remain enabled for an anonymous "
             "session (online Stop and software estop stay available)");
}

QTEST_MAIN(VisibleDisabledReasonsTest)
#include "visible_disabled_reasons_test.moc"
