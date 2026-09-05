#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QVector>

#include "application/permission_policy.h"

class QStackedWidget;
class QPushButton;

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
// Seven production pages are registered in the same order as NavPanel.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Application supplies its shared ShellModel. Tests and standalone users
    // may omit it, in which case the window owns a private model.
    explicit MainWindow(QWidget *parent = nullptr, ShellModel *model = nullptr);
    ~MainWindow() override;

    ShellModel *shellModel() const { return m_model; }

    // --- test/inspection API -------------------------------------------------
    int navItemCount() const;
    int currentPageIndex() const;
    QString topBarText() const;
    QString currentPageTitle() const;
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

protected:
    // Top-level widgets (incl. QMainWindow) receive QEvent::WindowDeactivate
    // but child widgets never do. Handling it here makes the §10.7 release
    // path work in the real app (a HoldButton on a page is never a top-level).
    bool event(QEvent *event) override;

public slots:
    // Switches page and clears hold intents (spec §10.7).
    void setCurrentPage(int index);

signals:
    // The owner (app wiring) connects these to the ControlCoordinator.
    void commandRequested(Command cmd);
    void modeSwitchRequested(bool autoMode);
    void loginLogoutRequested();

private:
    void buildLayout();
    void createPages();
    void loadTheme();

    ShellModel *m_model = nullptr;
    TopBar *m_topBar = nullptr;
    AlarmBanner *m_alarmBanner = nullptr;
    NavPanel *m_nav = nullptr;
    ActionBar *m_actions = nullptr;
    QStackedWidget *m_pages = nullptr;
    QVector<QPointer<HoldButton>> m_holdWidgets;
};

} // namespace hlm
