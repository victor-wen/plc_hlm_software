#pragma once

#include <QMainWindow>
#include <QVector>

#include "application/permission_policy.h"

class QStackedWidget;
class QPushButton;
class QLabel;

namespace hlm {

class ShellModel;
class TopBar;
class AlarmBanner;
class NavPanel;
class ActionBar;
class HoldButton;

// Application shell (spec §11.1): full-screen layout, 1920x1080 baseline,
// Qt Layout only (no absolute coordinates). Regions:
//   - TopBar (~72 px) + AlarmBanner (~40 px)
//   - NavPanel (~176 px, 7 items) | central page stack | ActionBar (~192 px)
//
// Intent clearing (spec §10.7): page switches and modal dialog triggers go
// through this class, which cancels all registered hold widgets. Pages and
// dialogs register their HoldButtons via registerHoldWidget().
//
// Pages: Tasks 11-17 plug real pages into the registry; for now 7 stub
// placeholder pages exist so navigation works and is testable.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    ShellModel *shellModel() const { return m_model; }

    // --- test/inspection API -------------------------------------------------
    int navItemCount() const;
    int currentPageIndex() const;
    QString topBarText() const;
    QString alarmBannerText() const;
    int navItemMinimumHeight() const;

    QPushButton *estopButton() const;   // from ActionBar
    QPushButton *startButton() const;
    QPushButton *stopButton() const;
    QPushButton *resetButton() const;

    // --- hold-intent registry (spec §10.7) ------------------------------------
    // Pages register their HoldButtons here; page switches, modal dialogs and
    // logout cancel all active holds.
    void registerHoldWidget(HoldButton *button);
    void clearHoldIntents();
    bool hasActiveHolds() const;

public slots:
    // Switches page and clears hold intents (spec §10.7).
    void setCurrentPage(int index);

signals:
    // The owner (app wiring) connects these to the ControlCoordinator.
    void commandRequested(Command cmd);
    void loginLogoutRequested();

private:
    void buildLayout();
    void createStubPages();

    ShellModel *m_model = nullptr;
    TopBar *m_topBar = nullptr;
    AlarmBanner *m_alarmBanner = nullptr;
    NavPanel *m_nav = nullptr;
    ActionBar *m_actions = nullptr;
    QStackedWidget *m_pages = nullptr;
    QVector<HoldButton *> m_holdWidgets;
};

} // namespace hlm