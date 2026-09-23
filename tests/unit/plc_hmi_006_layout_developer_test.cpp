// PLC-HMI-006 D6: developer-owned regression tests for the responsive reflow
// seams (not a restatement of the independent acceptance file
// tests/unit/plc_hmi_006_responsive_envelope_test.cpp).
//
// Locked invariants:
//   * the window minimum allows the smallest supported logical envelope,
//     683x384 (1366x768 at 200% DPI), to be presented exactly;
//   * the pinned safety strip keeps Stop and the software estop visible,
//     >= 48 px tall and outside the scrollable action group, so they never
//     depend on scrolling;
//   * the scrollable action group's viewport stays inside the window, so the
//     non-safety actions remain reachable;
//   * every page content widget is wrapped in a page scroll area whose
//     viewport stays inside the window;
//   * the top bar presents the single wide row while that row fits and
//     reflows into the compact two-row presentation below its fit width
//     (measured 630 px offscreen), hiding only the wide-only decorations.
//
// Runs offscreen like the other GUI tests (see tests/unit/CMakeLists.txt).

#include <QtTest>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QWidget>

#include "ui/MainWindow.h"
#include "ui/shell/top_bar.h"

using namespace hlm;

namespace {

bool fullyInside(const QWidget *window, const QWidget *widget)
{
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0));
    return window->rect().contains(QRect(topLeft, widget->size()));
}

} // namespace

class PlcHmi006LayoutDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void compactEnvelopeIsPresentableWithTheSafetyStripPinned();
    void actionRailAndPagesStayReachableInTheCompactEnvelope();
    void topBarUsesTheWideRowWhileItFitsAndCompactsBelowIt();
};

void PlcHmi006LayoutDeveloperTest::compactEnvelopeIsPresentableWithTheSafetyStripPinned()
{
    MainWindow w;
    w.show();
    QApplication::processEvents();
    w.resize(683, 384);
    QApplication::processEvents();

    // The window minimum must not force a size above the 200%-DPI logical
    // envelope, which would clip it instead of reflowing.
    QCOMPARE(w.size(), QSize(683, 384));

    auto *scroll = w.findChild<QScrollArea *>(QStringLiteral("actionBarScroll"));
    auto *strip = w.findChild<QWidget *>(QStringLiteral("actionBarSafetyStrip"));
    QVERIFY(scroll != nullptr);
    QVERIFY(strip != nullptr);
    QVERIFY(strip->isVisible());
    QVERIFY(fullyInside(&w, strip));

    for (QPushButton *button : {w.stopButton(), w.estopButton()}) {
        QVERIFY(button != nullptr);
        QVERIFY(button->isVisible());
        QVERIFY2(button->height() >= 48, qPrintable(button->objectName()));
        QVERIFY2(strip->isAncestorOf(button), qPrintable(button->objectName()));
        QVERIFY2(!scroll->isAncestorOf(button), qPrintable(button->objectName()));
        QVERIFY2(fullyInside(&w, button), qPrintable(button->objectName()));
    }
}

void PlcHmi006LayoutDeveloperTest::actionRailAndPagesStayReachableInTheCompactEnvelope()
{
    MainWindow w;
    w.show();
    QApplication::processEvents();
    w.resize(683, 384);
    QApplication::processEvents();

    // The scrollable action group's viewport must stay inside the window, so
    // the non-safety actions remain reachable by scrolling.
    auto *scroll = w.findChild<QScrollArea *>(QStringLiteral("actionBarScroll"));
    QVERIFY(scroll != nullptr);
    QVERIFY(scroll->viewport() != nullptr);
    QVERIFY(scroll->viewport()->isVisible());
    QVERIFY(fullyInside(&w, scroll->viewport()));

    // Every page is wrapped in a page scroll area; the page content itself is
    // the scrollable widget, not a direct stack child. The current page's
    // viewport stays inside the window, so its content is reachable.
    auto *stack = w.findChild<QStackedWidget *>();
    QVERIFY(stack != nullptr);
    // 8 pages: the 7 production pages + 扫码服务 (user decision 2026-09-23).
    QCOMPARE(stack->count(), 8);
    for (int i = 0; i < stack->count(); ++i) {
        auto *wrap = qobject_cast<QScrollArea *>(stack->widget(i));
        QVERIFY2(wrap != nullptr, qPrintable(QStringLiteral("page %1").arg(i)));
        QVERIFY(wrap->widget() != nullptr);
        QCOMPARE(wrap->widget(), w.pageWidget(i));
    }
    auto *pageWrap = qobject_cast<QScrollArea *>(stack->currentWidget());
    QVERIFY(pageWrap != nullptr);
    QVERIFY(pageWrap->viewport()->isVisible());
    QVERIFY(fullyInside(&w, pageWrap->viewport()));
}

void PlcHmi006LayoutDeveloperTest::topBarUsesTheWideRowWhileItFitsAndCompactsBelowIt()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    auto *topBar = w.findChild<TopBar *>(QStringLiteral("topBar"));
    QVERIFY(topBar != nullptr);

    // Wide mode while the single status row fits: wide-only decorations are
    // visible and the row width requirement is satisfied.
    QVERIFY(!topBar->compact());
    QVERIFY(topBar->width() >= topBar->wideRowMinimumWidth());
    QVERIFY(w.findChild<QLabel *>(QStringLiteral("appName"))->isVisible());

    // The mode is a pure fit rule. At 683x384 the wide row still fits
    // (measured wideRowMinimumWidth() == 630 <= 683), so the bar must present
    // it rather than claim a compact mode it does not need; nothing is clipped.
    w.resize(683, 384);
    QApplication::processEvents();
    QCOMPARE(topBar->compact(), topBar->width() < topBar->wideRowMinimumWidth());

    // Below the fit width the compact two-row presentation is engaged: the
    // wide-only decorations disappear while the page title and all six status
    // chips stay visible.
    const int narrowWidth = topBar->wideRowMinimumWidth() - 1;
    QVERIFY(narrowWidth > w.minimumWidth()); // pure-width probe precondition
    w.resize(narrowWidth, 384);
    QApplication::processEvents();
    QVERIFY(topBar->compact());
    QVERIFY(narrowWidth < topBar->wideRowMinimumWidth());
    QVERIFY(!w.findChild<QLabel *>(QStringLiteral("appName"))->isVisible());
    QVERIFY(!w.findChild<QLabel *>(QStringLiteral("userLabel"))->isVisible());
    QVERIFY(w.findChild<QLabel *>(QStringLiteral("pageTitle"))->isVisible());
    const QList<QWidget *> lights =
        w.findChildren<QWidget *>(QStringLiteral("topStatusLight"));
    QCOMPARE(lights.size(), 6);
    for (QWidget *light : lights)
        QVERIFY(light->isVisible());
}

QTEST_MAIN(PlcHmi006LayoutDeveloperTest)
#include "plc_hmi_006_layout_developer_test.moc"
