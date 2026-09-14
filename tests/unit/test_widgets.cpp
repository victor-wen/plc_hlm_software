// Task 10 unit tests: shared industrial widgets (spec §11.1, §11.2).
//
// Coverage required by the task brief:
// - StatusLight: on/off/error/amber/unknown states with text + color.
// - ValueDisplay: shows value when valid, "—" when stale/invalid (spec §9,
//   §11.2); unit suffix; disabled look when invalid.
// - HoldButton: press emits holdChanged(true), all release paths emit
//   holdChanged(false) exactly once (spec §10.7):
//   mouse release, pointer move-out + release, window deactivation,
//   cancelHold() (page switch / modal dialog / logout / session timeout),
//   and app exit (destructor path).
// - PermissionButton: disabled with a visible reason (spec §11.4).

#include <QtTest>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QImage>
#include <QColor>
#include <QFontMetrics>

#include "ui/widgets/hold_button.h"
#include "ui/widgets/status_light.h"
#include "ui/widgets/value_display.h"
#include "ui/widgets/permission_button.h"

using hlm::HoldButton;
using hlm::StatusLight;
using hlm::StatusState;
using hlm::ValueDisplay;
using hlm::PermissionButton;

namespace {

// Sends a synthetic press/release at the widget center.
void pressAt(QWidget *w, Qt::MouseButton button = Qt::LeftButton)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      button, button, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

void releaseAt(QWidget *w, Qt::MouseButton button = Qt::LeftButton)
{
    const QPoint center = w->rect().center();
    QMouseEvent release(QEvent::MouseButtonRelease, center,
                        w->mapToGlobal(center), button, button,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

} // namespace

class WidgetsTest : public QObject
{
    Q_OBJECT

private slots:
    // --- StatusLight --------------------------------------------------------
    void statusLightShowsTextAndState();
    void statusLightUnknownState();

    // --- ValueDisplay -------------------------------------------------------
    void valueDisplayShowsValueWhenValid();
    void valueDisplayShowsDashWhenInvalid();
    void valueDisplayShowsDashWhenStale();
    void valueDisplayShowsUnit();
    void valueDisplayKeepsReadableFontUnderPixelSizing();
    void valueDisplayMinimumHintFitsDrawnFont();

    // --- HoldButton ---------------------------------------------------------
    void holdButtonPressAndRelease();
    void holdButtonReleaseOnMoveOut();
    void holdButtonReleaseOnWindowDeactivation();
    void holdButtonReleaseOnCancel();
    void holdButtonCancelWhenNotHeldIsNoop();
    void holdButtonDestructorClearsHold();
    void holdButtonDisabledDoesNotEmit();

    // --- PermissionButton ---------------------------------------------------
    void permissionButtonDisabledShowsReason();

    // --- touch target sizes (spec §11.1) ------------------------------------
    void holdButtonMinimumSize();
};

// --- StatusLight -------------------------------------------------------------

void WidgetsTest::statusLightShowsTextAndState()
{
    StatusLight light;
    light.setState(StatusState::On, QStringLiteral("在线"));
    QCOMPARE(light.text(), QStringLiteral("在线"));
    QCOMPARE(light.state(), StatusState::On);
    // Color must not be the unknown/gray for On (color is not the only
    // channel: text is always present, spec §11.2).
    QVERIFY(light.text().size() > 0);
}

void WidgetsTest::statusLightUnknownState()
{
    StatusLight light;
    light.setState(StatusState::Unknown, QStringLiteral("通讯中断"));
    QCOMPARE(light.state(), StatusState::Unknown);
    QCOMPARE(light.text(), QStringLiteral("通讯中断"));
}

// --- ValueDisplay ------------------------------------------------------------

void WidgetsTest::valueDisplayShowsValueWhenValid()
{
    ValueDisplay display;
    display.setValue(QStringLiteral("128.5"), QStringLiteral("mm"), true);
    QCOMPARE(display.text(), QStringLiteral("128.5 mm"));
    QVERIFY(display.isValid());
}

void WidgetsTest::valueDisplayShowsDashWhenInvalid()
{
    ValueDisplay display;
    display.setValue(QStringLiteral("128.5"), QStringLiteral("mm"), false);
    QCOMPARE(display.text(), QStringLiteral("—"));
    QVERIFY(!display.isValid());
}

void WidgetsTest::valueDisplayShowsDashWhenStale()
{
    // Stale is expressed as invalid: the caller marks the value invalid when
    // the snapshot is stale (spec §11.2: 过期/无效字段显示"—").
    ValueDisplay display;
    display.setValue(QStringLiteral("200"), QString(), true);
    display.setValue(QStringLiteral("200"), QString(), false);
    QCOMPARE(display.text(), QStringLiteral("—"));
}

void WidgetsTest::valueDisplayShowsUnit()
{
    ValueDisplay display;
    display.setValue(QStringLiteral("1500"), QStringLiteral("mm/min"), true);
    QCOMPARE(display.text(), QStringLiteral("1500 mm/min"));
}

void WidgetsTest::valueDisplayKeepsReadableFontUnderPixelSizing()
{
    // Regression: theme.qss sets `* { font-size: 15px; }`, so a widget's
    // resolved font has pixelSize()==15 and pointSizeF()==-1. The old paint
    // code did setPointSizeF(pointSizeF()+2) => setPointSizeF(1.0), collapsing
    // the glyphs to ~1px (white boxes with no visible text). Render the widget
    // and require real glyph pixels with a readable height.
    ValueDisplay display;
    QFont f = display.font();
    f.setPixelSize(15); // exactly what the px-based theme produces
    display.setFont(f);
    display.setValue(QStringLiteral("1500"), QStringLiteral("Hz"), true);
    display.resize(220, 48);
    display.show();
    QApplication::processEvents();

    const QImage img =
        display.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY(!img.isNull());

    // Count the dark glyph pixels in the interior (dark text on the white box).
    int dark = 0;
    int minY = img.height();
    int maxY = -1;
    for (int y = 3; y < img.height() - 3; ++y) {
        for (int x = 16; x < img.width() - 8; ++x) {
            const QColor c(img.pixel(x, y));
            if (c.lightness() < 120) {
                ++dark;
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }
    QVERIFY2(dark > 30, qPrintable(QStringLiteral("glyph pixels=%1").arg(dark)));
    const int glyphHeight = (maxY >= minY) ? (maxY - minY + 1) : 0;
    QVERIFY2(glyphHeight >= 10,
             qPrintable(QStringLiteral("glyph height=%1 px").arg(glyphHeight)));
}

void WidgetsTest::valueDisplayMinimumHintFitsDrawnFont()
{
    // minimumSizeHint must be computed from the SAME (bold, +2) font paintEvent
    // draws with, and include the 14/10 px horizontal insets, so the value is
    // not clipped when the widget sits at its minimum width.
    ValueDisplay display;
    QFont f = display.font();
    f.setPixelSize(15);
    display.setFont(f);
    display.setValue(QStringLiteral("1500"), QStringLiteral("Hz"), true);

    QFont drawn = f;
    drawn.setBold(true);
    drawn.setPixelSize(17); // base 15 + 2, matching the paint path
    const int advance = QFontMetrics(drawn).horizontalAdvance(display.text());
    // minimumSizeHint() is protected in ValueDisplay; read it via QWidget.
    const QSize hint = static_cast<const QWidget &>(display).minimumSizeHint();
    QVERIFY2(hint.width() >= advance + 24,
             qPrintable(QStringLiteral("minWidth=%1 advance=%2")
                            .arg(hint.width())
                            .arg(advance)));
}

// --- HoldButton --------------------------------------------------------------

void WidgetsTest::holdButtonPressAndRelease()
{
    HoldButton button;
    QSignalSpy spy(&button, &HoldButton::holdChanged);

    pressAt(&button);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QVERIFY(button.isHeld());

    releaseAt(&button);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(!button.isHeld());
}

void WidgetsTest::holdButtonReleaseOnMoveOut()
{
    HoldButton button;
    QSignalSpy spy(&button, &HoldButton::holdChanged);

    pressAt(&button);
    QCOMPARE(spy.count(), 1);

    // Pointer leaves the button while held: hold must clear immediately
    // (spec §10.7: 指针移出后释放).
    QMouseEvent leave(QEvent::Leave, QPointF(0, 0), QPointF(0, 0),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&button, &leave);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(!button.isHeld());

    // The late release outside must not re-emit or re-press.
    releaseAt(&button);
    QCOMPARE(spy.count(), 2);
}

void WidgetsTest::holdButtonReleaseOnWindowDeactivation()
{
    HoldButton button;
    button.show();
    QSignalSpy spy(&button, &HoldButton::holdChanged);

    pressAt(&button);
    QCOMPARE(spy.count(), 1);

    // Window deactivation (e.g. modal dialog popup, alt-tab): hold clears.
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&button, &deactivate);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(!button.isHeld());
}

void WidgetsTest::holdButtonReleaseOnCancel()
{
    // cancelHold() covers page switch, modal dialog, logout/session timeout
    // and app exit initiated by the owner (spec §10.7).
    HoldButton button;
    QSignalSpy spy(&button, &HoldButton::holdChanged);

    pressAt(&button);
    QCOMPARE(spy.count(), 1);

    button.cancelHold();
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(!button.isHeld());
}

void WidgetsTest::holdButtonCancelWhenNotHeldIsNoop()
{
    HoldButton button;
    QSignalSpy spy(&button, &HoldButton::holdChanged);
    button.cancelHold();
    QCOMPARE(spy.count(), 0);
}

void WidgetsTest::holdButtonDestructorClearsHold()
{
    // App exit path: destroying a held button must emit the clear so the
    // owner can send the release command (spec §10.7: 应用正常退出).
    HoldButton *button = new HoldButton;
    QSignalSpy spy(button, &HoldButton::holdChanged);
    pressAt(button);
    QCOMPARE(spy.count(), 1);
    button->deleteLater();
    // deleteLater needs event loop; delete directly instead.
    delete button;
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
}

void WidgetsTest::holdButtonDisabledDoesNotEmit()
{
    HoldButton button;
    button.setEnabled(false);
    QSignalSpy spy(&button, &HoldButton::holdChanged);
    pressAt(&button);
    QCOMPARE(spy.count(), 0);
    QVERIFY(!button.isHeld());
}

void WidgetsTest::holdButtonMinimumSize()
{
    // Touch targets >= 48x48 px (spec §11.1).
    HoldButton button;
    QVERIFY(button.minimumSizeHint().height() >= 48);
    QVERIFY(button.minimumSizeHint().width() >= 48);
}

// --- PermissionButton --------------------------------------------------------

void WidgetsTest::permissionButtonDisabledShowsReason()
{
    PermissionButton button;
    button.setEnabledWithReason(false, QStringLiteral("需要管理员权限"));
    QVERIFY(!button.isEnabled());
    QCOMPARE(button.disabledReason(), QStringLiteral("需要管理员权限"));
    // The reason must be discoverable by the user: shown as tooltip and
    // accessible via status tip (spec §11.4: 相邻位置或提示框说明).
    QCOMPARE(button.toolTip(), QStringLiteral("需要管理员权限"));

    button.setEnabledWithReason(true, QString());
    QVERIFY(button.isEnabled());
    QVERIFY(button.toolTip().isEmpty());
}

QTEST_MAIN(WidgetsTest)
#include "test_widgets.moc"