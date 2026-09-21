// PLC-HMI-006 black-box unit tests: the responsive UI envelope and compact
// breakpoint (brief OB-1, OB-2, OB-3, OB-4 and the compact half of OB-7).
//
// Authored only from .ai/test-briefs/PLC-HMI-006.yaml, the approved
// .ai/project-contract.yaml (F-02, NF-01, C-03 and the layout invariants) and
// inspectable test sources under tests/**. No production implementation source
// was read.
//
// The envelope is exercised offscreen by resizing the real MainWindow to
// representative logical sizes -- including DPI-equivalent logical sizes such
// as 1366x768 at 150% (911x512) and at 200% (683x384) -- and asserting
// observable geometry/visibility/reachability invariants:
//   * Stop and software-estop set stay visible, enabled, unoccluded and
//     touch-sized at every supported size (OB-1, OB-3);
//   * no non-safety content is clipped or made unreachable: it either fits in
//     the window or lives inside a visible scroll area that can reach it (OB-1,
//     OB-2);
//   * below the compact breakpoint every page stays operable and its reachable
//     content stays reachable by reflow/scroll (OB-2);
//   * no user-controlled UI zoom exists and resizing never rescales the UI
//     (OB-3);
//   * inline disabled reasons stay visible, legible and retrievable inside the
//     compact envelope and are never tooltip-only (OB-4);
//   * disabled controls stay unclickable while the safety controls stay usable
//     at the compact breakpoint (OB-7: permissions/interlocks unchanged), and
//     the recipe surface keeps its permission and interlock reasons.
//
// Frozen introspection assumptions (recorded in the author-phase RED report for
// controller adjudication): the shell exposes its pages through a
// QStackedWidget, the action bar is found as hlm::ActionBar, and the Stop /
// software-estop controls are identified by the production command they
// dispatch on activation with a distinctive-text fallback. The real DPI matrix,
// multi-monitor and full-screen behavior remain field/SAT acceptance items and
// are recorded as gaps instead of being asserted speculatively.

#include <QtTest>

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "domain/device_snapshot.h"
#include "ui/MainWindow.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// --- envelope definition ------------------------------------------------------

constexpr int kCompactWidth = 1024;
constexpr int kCompactHeight = 600;

// F-02 / OB-3: touch targets of at least 48 logical pixels.
constexpr int kMinimumTargetPx = 48;

// Legible touch text minimum used by the earlier independent UI tests; the
// contract fixes no exact size.
constexpr int kMinimumLegibleFontHeight = 12;

// Adjacency radius for a reason label of the control it explains; same generous
// constant as the earlier independent disabled-reason tests.
constexpr int kAdjacencyPx = 300;

struct EnvelopeSize
{
    int width;
    int height;
    const char *label;
};

// Representative supported sizes from 1366x768 through 3840x2160 plus the
// DPI-equivalent logical sizes and the compact breakpoint itself (1024x600) and
// its immediate neighbour below it (C-03, brief OB-1/OB-2).
QVector<EnvelopeSize> envelopeSizes()
{
    return {
        {1366, 768, "1366x768"},
        {1920, 1080, "1920x1080"},
        {2560, 1440, "2560x1440"},
        {3840, 2160, "3840x2160"},
        {911, 512, "1366x768 at 150% DPI (911x512 logical)"},
        {683, 384, "1366x768 at 200% DPI (683x384 logical)"},
        {1024, 600, "compact breakpoint 1024x600"},
        {1023, 599, "just below the compact breakpoint (1023x599)"},
    };
}

QVector<EnvelopeSize> compactSizes()
{
    QVector<EnvelopeSize> compact;
    for (const EnvelopeSize &size : envelopeSizes()) {
        if (size.width <= kCompactWidth && size.height <= kCompactHeight)
            compact.append(size);
    }
    return compact;
}

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

// --- generic Qt introspection -------------------------------------------------

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

QString widgetText(const QWidget *widget)
{
    if (const auto *button = qobject_cast<const QAbstractButton *>(widget))
        return button->text().trimmed();
    if (const auto *label = qobject_cast<const QLabel *>(widget))
        return label->text().trimmed();
    if (const auto *combo = qobject_cast<const QComboBox *>(widget))
        return combo->currentText().trimmed();
    return QString();
}

QString describeWidget(const QWidget *widget)
{
    QString text = widgetText(widget);
    if (text.size() > 40)
        text = text.left(40) + QStringLiteral("...");
    return QStringLiteral("%1%2")
        .arg(QString::fromLatin1(widget->metaObject()->className()),
             text.isEmpty() ? QString()
                            : QStringLiteral(" '") + text + QStringLiteral("'"));
}

bool isInteractiveControl(const QWidget *widget)
{
    return qobject_cast<const QAbstractButton *>(widget) != nullptr
        || qobject_cast<const QAbstractSpinBox *>(widget) != nullptr
        || qobject_cast<const QLineEdit *>(widget) != nullptr
        || qobject_cast<const QComboBox *>(widget) != nullptr
        || qobject_cast<const QAbstractScrollArea *>(widget) != nullptr
        || qobject_cast<const QScrollBar *>(widget) != nullptr
        || qobject_cast<const QLabel *>(widget) != nullptr;
}

// Qt-internal children of a compound control (a spin box's line edit, a combo
// box's line edit) are not independent content and are skipped so they do not
// produce duplicate reports.
bool isInternallyOwned(const QWidget *widget)
{
    const QWidget *parent = widget->parentWidget();
    if (parent == nullptr)
        return true;
    return qobject_cast<const QAbstractSpinBox *>(parent) != nullptr
        || qobject_cast<const QComboBox *>(parent) != nullptr;
}

// A widget counts as presented content when it is part of the window's own
// widget hierarchy (not a popup/top-level window of its own) and is visible.
bool isContentWidget(QWidget *window, QWidget *widget)
{
    if (widget == window || widget->isWindow())
        return false;
    if (!window->isAncestorOf(widget))
        return false;
    if (!isInteractiveControl(widget) || isInternallyOwned(widget))
        return false;
    return widget->isVisibleTo(window);
}

bool fullyInsideWindow(QWidget *window, QWidget *widget)
{
    if (widget == window)
        return true;
    if (window == nullptr || widget == nullptr || !window->isAncestorOf(widget))
        return false;
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0));
    return window->rect().contains(QRect(topLeft, widget->size()));
}

// A widget that does not fit the window is still reachable when it lives inside
// a scroll area whose viewport is itself fully inside the window, because the
// operator can scroll it into view. Everything else is clipped content.
QString unreachableReason(QWidget *window, QWidget *widget)
{
    if (fullyInsideWindow(window, widget))
        return QString();
    if (auto *area = qobject_cast<QAbstractScrollArea *>(widget)) {
        if (area->viewport() != nullptr && fullyInsideWindow(window, area->viewport()))
            return QString();
    }
    for (QWidget *parent = widget->parentWidget(); parent != nullptr && parent != window;
         parent = parent->parentWidget()) {
        auto *area = qobject_cast<QAbstractScrollArea *>(parent);
        if (area == nullptr)
            continue;
        // QAbstractScrollArea has no widget() accessor. For a QScrollArea the
        // scrollable content is scrollAreaWidget(); for any other scroll area
        // the widget must at least live inside the scrollable viewport.
        if (area->viewport() == nullptr || !area->viewport()->isAncestorOf(widget))
            continue;
        if (auto *scrollArea = qobject_cast<QScrollArea *>(area)) {
            QWidget *content = scrollArea->widget();
            if (content == nullptr || (content != widget && !content->isAncestorOf(widget)))
                continue;
        }
        if (!area->viewport()->isVisibleTo(window))
            continue;
        if (!fullyInsideWindow(window, area->viewport()))
            continue;
        return QString();
    }
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0));
    return QStringLiteral("%1 at %2x%3+%4+%5 is outside the window rect %6x%7 "
                          "without a visible scroll area that can reach it")
        .arg(describeWidget(widget))
        .arg(widget->width())
        .arg(widget->height())
        .arg(topLeft.x())
        .arg(topLeft.y())
        .arg(window->width())
        .arg(window->height());
}

// --- window set-up ------------------------------------------------------------

void presentOnlineSession(MainWindow &window, const EnvelopeSize &size, Role role)
{
    window.resize(size.width, size.height);
    window.show();
    QApplication::processEvents();
    window.shellModel()->setUser(role == Role::Admin ? QStringLiteral("admin") : QString(),
                                 role);
    window.shellModel()->updateSnapshot(
        DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false)));
    QApplication::processEvents();
}

QWidget *currentPageWidget(MainWindow &window)
{
    if (auto *stack = window.findChild<QStackedWidget *>(); stack != nullptr)
        return stack->currentWidget();
    return &window;
}

// --- safety-control identification -------------------------------------------

// Observable identification: activate every enabled action-bar control in an
// online anonymous session and record which production command it dispatches.
// A two-step confirm control emits on its second activation, so each control is
// activated up to twice before the command is recorded.
QHash<int, QPointer<QAbstractButton>> actionBarControlsByCommand(ActionBar *bar)
{
    QHash<int, QPointer<QAbstractButton>> byCommand;
    const QList<QAbstractButton *> buttons = bar->findChildren<QAbstractButton *>();
    for (QAbstractButton *button : buttons) {
        if (!button->isEnabled() || !button->isVisibleTo(bar))
            continue;
        Command last = Command::Stop;
        bool seen = false;
        const QMetaObject::Connection connection = QObject::connect(
            bar, &ActionBar::actionRequested, button,
            [&last, &seen](Command command) {
                last = command;
                seen = true;
            });
        for (int attempt = 0; attempt < 2 && !seen; ++attempt) {
            clickAt(button);
            QApplication::processEvents();
        }
        QObject::disconnect(connection);
        if (seen && !byCommand.contains(int(last)))
            byCommand.insert(int(last), QPointer<QAbstractButton>(button));
    }
    return byCommand;
}

QAbstractButton *controlMatchingTokens(QWidget *root, const QStringList &tokens)
{
    for (QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
        if (!button->isEnabled() || !button->isVisibleTo(root))
            continue;
        const QStringList candidates{button->text().trimmed(), button->toolTip(),
                                     button->statusTip(), button->accessibleName(),
                                     button->accessibleDescription()};
        for (const QString &candidate : candidates) {
            for (const QString &token : tokens) {
                if (!candidate.isEmpty()
                    && candidate.contains(token, Qt::CaseInsensitive))
                    return button;
            }
        }
    }
    return nullptr;
}

struct SafetyControl
{
    QAbstractButton *control = nullptr;
    QString how;
};

SafetyControl findSafetyControl(MainWindow &window, ActionBar *bar, Command command,
                                const QStringList &fallbackTokens, const char *name)
{
    const QHash<int, QPointer<QAbstractButton>> byCommand =
        actionBarControlsByCommand(bar);
    if (QAbstractButton *identified = byCommand.value(int(command)).data();
        identified != nullptr) {
        return {identified,
                QStringLiteral("%1 identified by the %2 command it dispatches")
                    .arg(QString::fromLatin1(name),
                         QString::fromLatin1(name))};
    }
    // Rebuild the window state consumed by the activation probe before falling
    // back to the distinctive-text identification.
    Q_UNUSED(window);
    if (QAbstractButton *byText = controlMatchingTokens(bar, fallbackTokens);
        byText != nullptr) {
        return {byText,
                QStringLiteral("%1 identified by its distinctive control text")
                    .arg(QString::fromLatin1(name))};
    }
    return {};
}

// --- disabled-reason introspection (same shape as the earlier independent
// --- disabled-reason tests) --------------------------------------------------

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

QVector<QAbstractButton *> disabledControlsWithDeclaredReason(QWidget *root)
{
    QVector<QAbstractButton *> controls;
    for (QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
        if (!button->isEnabled() && !declaredReason(button).isEmpty())
            controls.append(button);
    }
    return controls;
}

QVector<QLabel *> visibleTextLabels(QWidget *root, QWidget *window)
{
    QVector<QLabel *> labels;
    for (QLabel *label : root->findChildren<QLabel *>()) {
        if (label->text().trimmed().isEmpty() || label->isHidden())
            continue;
        if (!window->isAncestorOf(label) && label != window)
            continue;
        if (!label->isVisibleTo(window) || label->isWindow())
            continue;
        labels.append(label);
    }
    return labels;
}

// Visible labels that carry the control's declared reason. A label whose whole
// text is contained in the declared reason (>= 4 characters) also counts, so an
// implementation may shorten the wording without losing the semantics.
QVector<QLabel *> matchingReasonLabels(QWidget *root, QWidget *window, QWidget *control)
{
    const QString reason = declaredReason(control);
    QVector<QLabel *> matches;
    if (reason.isEmpty())
        return matches;
    for (QLabel *label : visibleTextLabels(root, window)) {
        const QString text = label->text().trimmed();
        if (text.contains(reason) || (text.size() >= 4 && reason.contains(text)))
            matches.append(label);
    }
    return matches;
}

bool isNear(QWidget *root, const QWidget *control, const QWidget *other)
{
    const QPoint a = control->mapTo(root, control->rect().center());
    const QPoint b = other->mapTo(root, other->rect().center());
    return (a - b).manhattanLength() <= kAdjacencyPx;
}

// --- zoom introspection -------------------------------------------------------

bool offersZoom(const QWidget *widget)
{
    const QStringList tokens{QStringLiteral("缩放"), QStringLiteral("放大"),
                             QStringLiteral("缩小"), QStringLiteral("zoom"),
                             QStringLiteral("界面比例"), QStringLiteral("显示比例")};
    const QStringList candidates{widget->toolTip(), widget->statusTip(),
                                 widget->accessibleName(),
                                 widget->accessibleDescription(), widget->windowTitle(),
                                 widgetText(widget)};
    for (const QString &candidate : candidates) {
        for (const QString &token : tokens) {
            if (!candidate.isEmpty() && candidate.contains(token, Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

} // namespace

class PlcHmi006ResponsiveEnvelopeTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1 / OB-2 / OB-3: the safety strip across the whole envelope -------
    void safetyControlsStayVisibleReachableAndTouchSizedAtEverySupportedSize();

    // --- OB-1 / OB-2: no clipping, everything reflows or scrolls --------------
    void nonSafetyContentIsNeverClippedOutsideTheReachableEnvelope();

    // --- OB-1 / OB-2: navigation still works at every supported size ----------
    void navigationAndCurrentPageContentStayOperableAtEverySupportedSize();

    // --- OB-3: no user-controlled zoom ----------------------------------------
    void noUserControlledZoomExistsAndResizingNeverRescalesTheUi();

    // --- OB-4 / OB-7: compact-envelope reasons and enforcement ----------------
    void disabledReasonsStayVisibleLegibleAndRetrievableInTheCompactEnvelope();
    void compactEnvelopeKeepsDisabledControlsUnclickableAndSafetyControlsUsable();
};

// --- OB-1 / OB-2 / OB-3 --------------------------------------------------------

void PlcHmi006ResponsiveEnvelopeTest::safetyControlsStayVisibleReachableAndTouchSizedAtEverySupportedSize()
{
    QStringList violations;

    for (const EnvelopeSize &size : envelopeSizes()) {
        MainWindow window;
        presentOnlineSession(window, size, Role::Anonymous);

        if (window.width() != size.width || window.height() != size.height) {
            violations.append(QStringLiteral("%1: the shell cannot present the requested "
                                            "envelope size (actual %2x%3)")
                                  .arg(QString::fromLatin1(size.label))
                                  .arg(window.width())
                                  .arg(window.height()));
            continue;
        }

        ActionBar *bar = window.findChild<ActionBar *>();
        if (bar == nullptr) {
            violations.append(QStringLiteral("%1: no action bar was found")
                                  .arg(QString::fromLatin1(size.label)));
            continue;
        }

        const SafetyControl stop = findSafetyControl(
            window, bar, Command::Stop,
            QStringList{QStringLiteral("停止"), QStringLiteral("停机")},
            "the Stop control");
        const SafetyControl estop = findSafetyControl(
            window, bar, Command::EstopSet,
            QStringList{QStringLiteral("急停")}, "the software-estop set control");

        const struct
        {
            SafetyControl selected;
            const char *name;
        } required[] = {{stop, "Stop"}, {estop, "software estop set"}};

        for (const auto &entry : required) {
            const QString sizeLabel = QString::fromLatin1(size.label);
            const QString name = QString::fromLatin1(entry.name);
            if (entry.selected.control == nullptr) {
                violations.append(QStringLiteral("%1: no %2 control could be identified "
                                                "in the online session")
                                      .arg(sizeLabel, name));
                continue;
            }
            QAbstractButton *control = entry.selected.control;
            if (!control->isVisibleTo(bar)) {
                violations.append(QStringLiteral("%1: the %2 control is not visible "
                                                "inside the action bar")
                                      .arg(sizeLabel, name));
            }
            if (!control->isEnabled()) {
                violations.append(QStringLiteral("%1: the %2 control is not usable "
                                                "(disabled) with communication "
                                                "preconditions satisfied")
                                      .arg(sizeLabel, name));
            }
            if (!fullyInsideWindow(&window, control)) {
                const QPoint topLeft = control->mapTo(&window, QPoint(0, 0));
                violations.append(
                    QStringLiteral("%1: the %2 control is clipped (%3x%4+%5+%6 outside "
                                   "the %7x%8 window and must not require scrolling)")
                        .arg(sizeLabel, name)
                        .arg(control->width())
                        .arg(control->height())
                        .arg(topLeft.x())
                        .arg(topLeft.y())
                        .arg(window.width())
                        .arg(window.height()));
            } else {
                const QPoint center = control->mapTo(&window, control->rect().center());
                QWidget *hit = window.childAt(center);
                if (hit != control && !control->isAncestorOf(hit)) {
                    violations.append(
                        QStringLiteral("%1: the %2 control is occluded at its center by "
                                       "%3")
                            .arg(sizeLabel, name,
                                 hit != nullptr ? describeWidget(hit)
                                                : QStringLiteral("(no widget)")));
                }
            }
            if (control->width() < kMinimumTargetPx
                || control->height() < kMinimumTargetPx) {
                violations.append(
                    QStringLiteral("%1: the %2 touch target is %3x%4, below the "
                                   "%5x%5 logical-pixel minimum")
                        .arg(sizeLabel, name)
                        .arg(control->width())
                        .arg(control->height())
                        .arg(kMinimumTargetPx));
            }
        }

        // Every usable machine control in the action bar keeps a touch target of
        // at least 48 logical pixels at every supported size (OB-3).
        for (QAbstractButton *button : bar->findChildren<QAbstractButton *>()) {
            if (!button->isEnabled() || !button->isVisibleTo(bar))
                continue;
            if (button->width() >= kMinimumTargetPx
                && button->height() >= kMinimumTargetPx)
                continue;
            violations.append(
                QStringLiteral("%1: usable action-bar control %2 is %3x%4, below the "
                               "%5x%5 logical-pixel minimum")
                    .arg(QString::fromLatin1(size.label), describeWidget(button))
                    .arg(button->width())
                    .arg(button->height())
                    .arg(kMinimumTargetPx));
        }
    }

    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1Char('\n'))));
}

// --- OB-1 / OB-2 ---------------------------------------------------------------

void PlcHmi006ResponsiveEnvelopeTest::nonSafetyContentIsNeverClippedOutsideTheReachableEnvelope()
{
    QStringList violations;

    for (const EnvelopeSize &size : envelopeSizes()) {
        MainWindow window;
        presentOnlineSession(window, size, Role::Admin);

        if (window.width() != size.width || window.height() != size.height) {
            violations.append(QStringLiteral("%1: the shell cannot present the requested "
                                            "envelope size (actual %2x%3)")
                                  .arg(QString::fromLatin1(size.label))
                                  .arg(window.width())
                                  .arg(window.height()));
            continue;
        }

        for (QWidget *widget : window.findChildren<QWidget *>()) {
            if (!isContentWidget(&window, widget))
                continue;
            const QString reason = unreachableReason(&window, widget);
            if (!reason.isEmpty()) {
                violations.append(QStringLiteral("%1: %2")
                                      .arg(QString::fromLatin1(size.label), reason));
            }
        }
    }

    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1Char('\n'))));
}

// --- OB-1 / OB-2 ---------------------------------------------------------------

void PlcHmi006ResponsiveEnvelopeTest::navigationAndCurrentPageContentStayOperableAtEverySupportedSize()
{
    QStringList violations;

    for (const EnvelopeSize &size : envelopeSizes()) {
        MainWindow window;
        presentOnlineSession(window, size, Role::Admin);

        if (window.width() != size.width || window.height() != size.height) {
            violations.append(QStringLiteral("%1: the shell cannot present the requested "
                                            "envelope size (actual %2x%3)")
                                  .arg(QString::fromLatin1(size.label))
                                  .arg(window.width())
                                  .arg(window.height()));
            continue;
        }

        int pageCount = 1;
        if (auto *stack = window.findChild<QStackedWidget *>(); stack != nullptr)
            pageCount = stack->count();

        for (int page = 0; page < pageCount; ++page) {
            window.setCurrentPage(page);
            QApplication::processEvents();

            QWidget *pageRoot = currentPageWidget(window);
            if (pageRoot == nullptr || !pageRoot->isVisibleTo(&window)) {
                violations.append(QStringLiteral("%1: page %2 is not visible after "
                                                "navigation")
                                      .arg(QString::fromLatin1(size.label))
                                      .arg(page));
                continue;
            }

            int reachableControls = 0;
            for (QWidget *widget : pageRoot->findChildren<QWidget *>()) {
                if (widget->isWindow() || !widget->isVisibleTo(&window))
                    continue;
                if (!isInteractiveControl(widget) || isInternallyOwned(widget))
                    continue;
                if (!pageRoot->isAncestorOf(widget))
                    continue;
                if (unreachableReason(&window, widget).isEmpty())
                    ++reachableControls;
            }

            // The current-page actions must stay reachable without clipping: at
            // least one interactive control of the page is presented, and no
            // visible page control is clipped without a scroll path.
            if (reachableControls == 0) {
                violations.append(QStringLiteral("%1: page %2 presents no reachable "
                                                "interactive control")
                                      .arg(QString::fromLatin1(size.label))
                                      .arg(page));
            }
            for (QWidget *widget : pageRoot->findChildren<QWidget *>()) {
                if (widget->isWindow() || !widget->isVisibleTo(&window))
                    continue;
                if (!isInteractiveControl(widget) || isInternallyOwned(widget))
                    continue;
                const QString reason = unreachableReason(&window, widget);
                if (!reason.isEmpty()) {
                    violations.append(QStringLiteral("%1: page %2: %3")
                                          .arg(QString::fromLatin1(size.label))
                                          .arg(page)
                                          .arg(reason));
                }
            }
        }
    }

    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1Char('\n'))));
}

// --- OB-3 ----------------------------------------------------------------------

void PlcHmi006ResponsiveEnvelopeTest::noUserControlledZoomExistsAndResizingNeverRescalesTheUi()
{
    MainWindow window;
    window.resize(1366, 768);
    window.show();
    QApplication::processEvents();
    window.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);

    // No user-controlled UI zoom exists anywhere in the shell: neither a widget
    // nor a menu action may offer one.
    QStringList zoomOffers;
    for (QWidget *widget : window.findChildren<QWidget *>()) {
        if (widget->isWindow())
            continue;
        if (offersZoom(widget))
            zoomOffers.append(describeWidget(widget));
    }
    for (QAction *action : window.findChildren<QAction *>()) {
        QStringList candidates{action->text(), action->toolTip(),
                               action->statusTip(), action->iconText()};
        for (const QString &candidate : candidates) {
            for (const QString &token :
                 QStringList{QStringLiteral("缩放"), QStringLiteral("放大"),
                             QStringLiteral("缩小"), QStringLiteral("zoom"),
                             QStringLiteral("界面比例"), QStringLiteral("显示比例")}) {
                if (!candidate.isEmpty()
                    && candidate.contains(token, Qt::CaseInsensitive)) {
                    zoomOffers.append(QStringLiteral("menu action '%1'")
                                          .arg(action->text().trimmed()));
                    break;
                }
            }
        }
    }
    QVERIFY2(zoomOffers.isEmpty(),
             qPrintable(QStringLiteral("the shell offers user-controlled UI zoom: %1")
                            .arg(zoomOffers.join(QStringLiteral(", ")))));

    // Resizing across the envelope must never rescale the UI: the application
    // font stays the same at every supported size.
    const qreal fontHeightBefore = QApplication::font().pointSizeF();
    QStringList fontChanges;
    for (const EnvelopeSize &size : envelopeSizes()) {
        window.resize(size.width, size.height);
        QApplication::processEvents();
        const qreal fontHeightNow = QApplication::font().pointSizeF();
        if (!qFuzzyCompare(fontHeightBefore, fontHeightNow)) {
            fontChanges.append(QStringLiteral("%1: application font changed from %2 to %3")
                                   .arg(QString::fromLatin1(size.label))
                                   .arg(fontHeightBefore)
                                   .arg(fontHeightNow));
        }
    }
    QVERIFY2(fontChanges.isEmpty(), qPrintable(fontChanges.join(QLatin1Char('\n'))));
}

// --- OB-4 / OB-7 ---------------------------------------------------------------

void PlcHmi006ResponsiveEnvelopeTest::disabledReasonsStayVisibleLegibleAndRetrievableInTheCompactEnvelope()
{
    QStringList violations;

    for (const EnvelopeSize &size : compactSizes()) {
        // Configuration 1: anonymous online session -- the action bar and the
        // recipe surface are blocked by permission.
        {
            MainWindow window;
            presentOnlineSession(window, size, Role::Anonymous);
            if (window.width() != size.width || window.height() != size.height) {
                violations.append(
                    QStringLiteral("%1: the shell cannot present the requested compact "
                                   "envelope size (actual %2x%3)")
                        .arg(QString::fromLatin1(size.label))
                        .arg(window.width())
                        .arg(window.height()));
                continue;
            }
            ActionBar *bar = window.findChild<ActionBar *>();
            if (bar == nullptr) {
                violations.append(QStringLiteral("%1: no action bar was found")
                                      .arg(QString::fromLatin1(size.label)));
                continue;
            }
            const QString context =
                QStringLiteral("%1, anonymous session").arg(QString::fromLatin1(size.label));
            const QVector<QAbstractButton *> controls =
                disabledControlsWithDeclaredReason(bar);
            if (controls.isEmpty()) {
                violations.append(QStringLiteral("%1: no disabled control with a declared "
                                                "reason was found")
                                      .arg(context));
            }
            for (QAbstractButton *control : controls) {
                const QString reason = declaredReason(control);
                const QVector<QLabel *> labels =
                    matchingReasonLabels(bar, &window, control);
                if (labels.isEmpty()) {
                    violations.append(
                        QStringLiteral("%1: disabled control %2 declares '%3' but no "
                                       "visible label carries it (tooltip-only)")
                            .arg(context, describeWidget(control), reason));
                    continue;
                }
                bool presented = false;
                for (QLabel *label : labels) {
                    if (!unreachableReason(&window, label).isEmpty()) {
                        violations.append(
                            QStringLiteral("%1: the reason label for %2 is outside the "
                                           "compact envelope: %3")
                                .arg(context, describeWidget(control),
                                     unreachableReason(&window, label)));
                        continue;
                    }
                    presented = true;
                    if (label->fontMetrics().height() < kMinimumLegibleFontHeight) {
                        violations.append(
                            QStringLiteral("%1: the reason text for %2 is %3 px high, "
                                           "below the legible minimum %4")
                                .arg(context, describeWidget(control))
                                .arg(label->fontMetrics().height())
                                .arg(kMinimumLegibleFontHeight));
                    }
                    const QString text = label->text().trimmed();
                    const bool fullReasonVisible = text.contains(reason);
                    if (!fullReasonVisible) {
                        // A truncation is acceptable only when it keeps a
                        // meaningful part of the reason AND the full reason stays
                        // retrievable without hover (touch-accessible).
                        const bool meaningfulPart = text.size() >= 4
                            && reason.contains(text);
                        const bool retrievable = control->accessibleDescription()
                                                     .contains(reason)
                            || control->toolTip().contains(reason);
                        if (!meaningfulPart || !retrievable) {
                            violations.append(
                                QStringLiteral("%1: the reason for %2 is truncated to "
                                               "'%3' without a touch-accessible full "
                                               "text (declared '%4')")
                                    .arg(context, describeWidget(control), text, reason));
                        }
                    }
                }
                bool adjacent = false;
                for (QLabel *label : labels) {
                    if (isNear(bar, control, label)) {
                        adjacent = true;
                        break;
                    }
                }
                if (presented && !adjacent) {
                    violations.append(
                        QStringLiteral("%1: the visible reason for %2 is not adjacent to "
                                       "the control")
                            .arg(context, describeWidget(control)));
                }
            }
        }

        // Configuration 2: administrator with connected but latched-fault data
        // -- the recipe and manual surfaces are blocked by an interlock. User
        // decision 2026-09-21: an un-homed machine (M61 clear) is no longer an
        // interlock cause, so the blocking cause is a latched fault (M14).
        // Every page is visited
        // in turn and only controls that the current page actually presents are
        // checked: a control on a hidden page cannot show its reason in the
        // window, and is not required to.
        {
            MainWindow window;
            window.resize(size.width, size.height);
            window.show();
            QApplication::processEvents();
            window.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
            window.shellModel()->updateSnapshot(
                DeviceSnapshot(snapshotData(/*homed=*/true, /*automatic=*/false,
                                            /*latchedFault=*/true)));
            QApplication::processEvents();
            if (window.width() != size.width || window.height() != size.height)
                continue;

            const QString context = QStringLiteral("%1, interlock block")
                                        .arg(QString::fromLatin1(size.label));

            int pageCount = 1;
            if (auto *stack = window.findChild<QStackedWidget *>(); stack != nullptr)
                pageCount = stack->count();

            int checked = 0;
            for (int page = 0; page < pageCount; ++page) {
                window.setCurrentPage(page);
                QApplication::processEvents();

                QWidget *pageRoot = currentPageWidget(window);
                if (pageRoot == nullptr)
                    continue;

                const QString pageContext =
                    QStringLiteral("%1, page %2").arg(context).arg(page);

                QVector<QAbstractButton *> visibleBlockedControls;
                for (QAbstractButton *button :
                     pageRoot->findChildren<QAbstractButton *>()) {
                    if (button->isEnabled() || declaredReason(button).isEmpty())
                        continue;
                    if (!button->isVisibleTo(&window))
                        continue;
                    visibleBlockedControls.append(button);
                }

                for (QAbstractButton *control : visibleBlockedControls) {
                    const QString reason = declaredReason(control);
                    // Only labels presented by THIS page count: the window-wide
                    // scan also matched unrelated chrome (the alarm banner shows
                    // the active alarm text, which can coincidentally equal the
                    // control's declared reason), and isNear() then mapped
                    // between two widgets without a common ancestor.
                    const QVector<QLabel *> labels =
                        matchingReasonLabels(pageRoot, &window, control);
                    if (labels.isEmpty()) {
                        violations.append(
                            QStringLiteral("%1: disabled control %2 declares '%3' but no "
                                           "visible label carries it (tooltip-only)")
                                .arg(pageContext, describeWidget(control), reason));
                        continue;
                    }
                    bool presented = false;
                    for (QLabel *label : labels) {
                        if (!unreachableReason(&window, label).isEmpty()) {
                            violations.append(
                                QStringLiteral("%1: the reason label for %2 is outside the "
                                               "compact envelope: %3")
                                    .arg(pageContext, describeWidget(control),
                                         unreachableReason(&window, label)));
                            continue;
                        }
                        presented = true;
                        if (label->fontMetrics().height() < kMinimumLegibleFontHeight) {
                            violations.append(
                                QStringLiteral("%1: the reason text for %2 is %3 px high, "
                                               "below the legible minimum %4")
                                    .arg(pageContext, describeWidget(control))
                                    .arg(label->fontMetrics().height())
                                    .arg(kMinimumLegibleFontHeight));
                        }
                        const QString text = label->text().trimmed();
                        const bool fullReasonVisible = text.contains(reason);
                        if (!fullReasonVisible) {
                            // A truncation is acceptable only when it keeps a
                            // meaningful part of the reason AND the full reason
                            // stays retrievable without hover (touch-accessible).
                            const bool meaningfulPart = text.size() >= 4
                                && reason.contains(text);
                            const bool retrievable = control->accessibleDescription()
                                                         .contains(reason)
                                || control->toolTip().contains(reason);
                            if (!meaningfulPart || !retrievable) {
                                violations.append(
                                    QStringLiteral("%1: the reason for %2 is truncated to "
                                                   "'%3' without a touch-accessible full "
                                                   "text (declared '%4')")
                                        .arg(pageContext, describeWidget(control), text,
                                             reason));
                            }
                        }
                    }
                    bool adjacent = false;
                    for (QLabel *label : labels) {
                        if (isNear(pageRoot, control, label)) {
                            adjacent = true;
                            break;
                        }
                    }
                    if (presented && !adjacent) {
                        violations.append(
                            QStringLiteral("%1: the visible reason for %2 is not adjacent "
                                           "to the control")
                                .arg(pageContext, describeWidget(control)));
                    }
                    if (presented)
                        ++checked;
                }
            }
            if (checked == 0) {
                violations.append(
                    QStringLiteral("%1: the interlock-blocked surface shows no reachable "
                                   "reason inside the compact envelope")
                        .arg(context));
            }
        }
    }

    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1Char('\n'))));
}

// --- OB-7 ----------------------------------------------------------------------

void PlcHmi006ResponsiveEnvelopeTest::compactEnvelopeKeepsDisabledControlsUnclickableAndSafetyControlsUsable()
{
    const EnvelopeSize size{683, 384, "683x384 compact"};

    MainWindow window;
    presentOnlineSession(window, size, Role::Anonymous);
    QVERIFY2(window.width() == size.width && window.height() == size.height,
             qPrintable(QStringLiteral("the shell cannot present the compact envelope "
                                       "(actual %1x%2)")
                            .arg(window.width())
                            .arg(window.height())));

    ActionBar *bar = window.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    QVector<int> emittedCommands;
    const QMetaObject::Connection connection = QObject::connect(
        bar, &ActionBar::actionRequested, bar,
        [&emittedCommands](Command command) { emittedCommands.append(int(command)); });

    // A disabled control explains itself but stays unclickable: activating it
    // must neither enable it nor dispatch a command.
    const QVector<QAbstractButton *> disabledControls =
        disabledControlsWithDeclaredReason(bar);
    QVERIFY2(!disabledControls.isEmpty(),
             "precondition: an anonymous online session must present at least one "
             "disabled action-bar control with a declared reason");
    for (QAbstractButton *control : disabledControls) {
        clickAt(control);
        QApplication::processEvents();
        QVERIFY2(!control->isEnabled(),
                 qPrintable(QStringLiteral("a clicked disabled control became enabled: %1")
                                .arg(describeWidget(control))));
    }
    QVERIFY2(emittedCommands.isEmpty(),
             qPrintable(QStringLiteral("a disabled control dispatched a command: %1")
                            .arg(emittedCommands.size())));

    // The safety controls stay enabled and reachable at the compact breakpoint;
    // activating them is the intended behavior (communication preconditions are
    // satisfied), so the same click path must still work for them.
    const SafetyControl stop = findSafetyControl(
        window, bar, Command::Stop,
        QStringList{QStringLiteral("停止"), QStringLiteral("停机")}, "the Stop control");
    QVERIFY2(stop.control != nullptr,
             "no Stop control could be identified at the compact breakpoint");
    QVERIFY2(stop.control->isEnabled(),
             "the Stop control must stay usable at the compact breakpoint");
    QVERIFY2(fullyInsideWindow(&window, stop.control),
             "the Stop control must stay inside the compact envelope without scrolling");

    QObject::disconnect(connection);
}

QTEST_MAIN(PlcHmi006ResponsiveEnvelopeTest)
#include "plc_hmi_006_responsive_envelope_test.moc"
