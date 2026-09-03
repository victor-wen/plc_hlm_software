#pragma once

#include <QPushButton>

namespace hlm {

// Hold (continuous) command button (spec §10.7): press writes 1, release
// writes 0. The button itself does NOT talk to the PLC; it emits
// holdChanged(bool) and the owner routes the command. The owner must
// connect this to the ControlCoordinator manualHold flow.
//
// ALL release paths emit holdChanged(false) exactly once (spec §10.7):
//   - mouse/touch release inside the button
//   - pointer moved out of the button (Leave) then released
//   - window deactivation (alt-tab, modal dialog taking focus)
//   - cancelHold() called by the owner: page switch, modal dialog popup,
//     logout/session timeout, app exit
//   - destruction while held (app exit path)
class HoldButton : public QPushButton
{
    Q_OBJECT

public:
    explicit HoldButton(QWidget *parent = nullptr);
    explicit HoldButton(const QString &text, QWidget *parent = nullptr);
    ~HoldButton() override;

    bool isHeld() const { return m_held; }

public slots:
    // Owner-initiated release: page switch, modal dialog, logout, app exit.
    // No-op when not held. Emits holdChanged(false) exactly once.
    void cancelHold();

signals:
    // pressed=true on press, false on ANY release path. The owner sends the
    // write 1 / write 0 command; the button never does I/O.
    void holdChanged(bool pressed);

public:
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void setHeld(bool held);

    bool m_held = false;
};

} // namespace hlm