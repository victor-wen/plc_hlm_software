#pragma once

#include <QWidget>
#include <QString>

namespace hlm {

// State semantics for status lights and status chips (spec §11.2).
// Color is never the only channel: every state also carries text.
enum class StatusState {
    Unknown = 0, // 离线/未知/过期: gray + "—"/text
    On,          // 在线/完成/准备: green
    Info,        // 自动模式/命令发送中: blue
    Amber,       // 手动/屏蔽/警告: amber
    Error,       // 故障/急停: red
};

// Shared industrial status light: a colored dot + text label. Plain widget,
// no I/O; the shell binds it to ShellModel state.
class StatusLight : public QWidget
{
    Q_OBJECT

public:
    explicit StatusLight(QWidget *parent = nullptr);

    void setState(StatusState state, const QString &text);
    StatusState state() const { return m_state; }
    QString text() const { return m_text; }

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize minimumSizeHint() const override;

private:
    StatusState m_state = StatusState::Unknown;
    QString m_text;
};

} // namespace hlm