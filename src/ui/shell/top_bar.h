#pragma once

#include <QWidget>
#include <QLabel>
#include <QVector>

#include "ui/widgets/status_light.h"

class QTimer;

namespace hlm {

class ShellModel;

// Top status bar, ~72 px (spec §11.1): app name, PLC online, hand/auto mode,
// running, homed, ready, fault/estop, current user, time. All statuses show
// text + color (color is never the only channel, spec §11.2).
class TopBar : public QWidget
{
    Q_OBJECT

public:
    explicit TopBar(ShellModel &model, QWidget *parent = nullptr);

    // Aggregated text of all status chips (for tests).
    QString text() const;
    QString pageTitle() const;

public slots:
    void refresh();
    void setPageTitle(const QString &title);

private:
    void buildLights();

    ShellModel &m_model;
    QLabel *m_appName = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_userLabel = nullptr;
    QLabel *m_clockLabel = nullptr;
    QVector<StatusLight *> m_lights; // online, mode, running, homed, ready, fault
    QTimer *m_clockTimer = nullptr;
};

} // namespace hlm
