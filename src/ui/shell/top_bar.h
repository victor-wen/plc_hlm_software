#pragma once

#include <QWidget>
#include <QLabel>
#include <QVector>

#include "ui/widgets/status_light.h"

class QGridLayout;
class QTimer;

namespace hlm {

class ShellModel;

// Top status bar, ~72 px (spec §11.1): app name, PLC online, hand/auto mode,
// running, homed, ready, fault/estop, current user, time. All statuses show
// text + color (color is never the only channel, spec §11.2).
//
// Responsive (PLC-HMI-006 D1/D2): the single status row is used while it fits
// the available width; below that the same information reflows into a compact
// two-row presentation (page title + clock, six status lights three per row)
// instead of clipping. The bar never forces a minimum width.
class TopBar : public QWidget
{
    Q_OBJECT

public:
    explicit TopBar(ShellModel &model, QWidget *parent = nullptr);

    // Aggregated text of all status chips (for tests).
    QString text() const;
    QString pageTitle() const;
    // True while the compact two-row presentation is active.
    bool compact() const { return m_compact; }
    // Width the single-row presentation needs; the compact presentation is
    // chosen while the bar is narrower than this (test/inspection seam).
    int wideRowMinimumWidth() const;

public slots:
    void refresh();
    void setPageTitle(const QString &title);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildLights();
    // Places the children in the presentation that fits the current width.
    void applyPresentation();
    QVector<QWidget *> wideRowWidgets() const;

    ShellModel &m_model;
    QGridLayout *m_grid = nullptr;
    QLabel *m_appName = nullptr;
    QLabel *m_divider = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_userLabel = nullptr;
    QLabel *m_clockLabel = nullptr;
    QVector<StatusLight *> m_lights; // online, mode, running, homed, ready, fault
    QTimer *m_clockTimer = nullptr;
    bool m_compact = false;
    bool m_presentationPlaced = false;
};

} // namespace hlm
