#include "ui/shell/action_bar.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"
#include "application/interlock_rules.h"

#include <QVBoxLayout>
#include <QStringList>
#include <QLabel>
#include <QStyle>
#include <QVariant>

namespace hlm {

namespace {
// Combines permission + interlock results into one reason string.
QString combinedReason(const PermissionResult &p, const InterlockResult &i)
{
    QStringList reasons;
    if (!p.allowed && !p.reason.isEmpty())
        reasons.append(p.reason);
    reasons.append(i.unmet);
    return reasons.join(QStringLiteral("；"));
}

void setActiveState(QWidget *widget, bool active)
{
    const QVariant current = widget->property("active");
    if (current.isValid() && current.toBool() == active)
        return;
    widget->setProperty("active", active);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}
} // namespace

ActionBar::ActionBar(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("actionBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(192);
    setMaximumWidth(192);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("设备操作"), this);
    title->setObjectName(QStringLiteral("actionBarTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto make = [this, layout](const QString &text) {
        auto *b = new PermissionButton(text, this);
        // 56 logical px remains a large touch target while keeping every
        // safety control visible on a 768 px-high / 125%-scaled display.
        b->setMinimumHeight(56);
        layout->addWidget(b);
        return b;
    };

    auto addGroupLabel = [this, layout](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setObjectName(QStringLiteral("actionGroupLabel"));
        layout->addWidget(label);
    };

    addGroupLabel(QStringLiteral("运行模式"));
    m_manual = make(QStringLiteral("手动"));
    m_manual->setObjectName(QStringLiteral("manualModeButton"));
    m_auto = make(QStringLiteral("自动"));
    m_auto->setObjectName(QStringLiteral("autoModeButton"));

    addGroupLabel(QStringLiteral("流程控制"));
    m_start = make(QStringLiteral("启动"));
    m_start->setObjectName(QStringLiteral("startButton"));
    m_stop = make(QStringLiteral("停止"));
    m_stop->setObjectName(QStringLiteral("stopButton"));
    m_reset = make(QStringLiteral("复位"));
    m_reset->setObjectName(QStringLiteral("resetButton"));

    addGroupLabel(QStringLiteral("当前账户"));
    m_login = make(QStringLiteral("登录"));
    m_login->setObjectName(QStringLiteral("loginButton"));

    // Software estop: separated from normal actions with a spacer, fixed red
    // danger style (spec §10.6). Any user may set it while online.
    layout->addStretch();
    m_estop = new PermissionButton(QStringLiteral("软件急停"), this);
    m_estop->setObjectName(QStringLiteral("estopButton"));
    m_estop->setProperty("danger", true);
    m_estop->setMinimumHeight(80);
    m_estop->setStyleSheet(
        QStringLiteral("QPushButton#estopButton { background-color: #c42b2b;"
                       " color: white; font-weight: bold; border: 3px solid #7a1010;"
                       " border-radius: 6px; }"
                       "QPushButton#estopButton:disabled { background-color: #8a5555; }"));
    layout->addWidget(m_estop);

    connect(m_manual, &QPushButton::clicked, this,
            [this] { emit modeSwitchRequested(false); });
    connect(m_auto, &QPushButton::clicked, this,
            [this] { emit modeSwitchRequested(true); });
    connect(m_start, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::Start); });
    connect(m_stop, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::Stop); });
    connect(m_reset, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::Reset); });
    connect(m_estop, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::EstopSet); });
    connect(m_login, &QPushButton::clicked, this,
            &ActionBar::loginLogoutRequested);

    connect(&m_model, &ShellModel::stateChanged, this, &ActionBar::refresh);
    refresh();
}

void ActionBar::refresh()
{
    const DeviceSnapshot &s = m_model.snapshot();
    const bool online = m_model.online();
    const Role role = m_model.role();

    // Active colors come exclusively from the last confirmed PLC snapshot.
    // A click must never make the mode/run state look successful before the
    // corresponding snapshot arrives (spec §11.2).
    setActiveState(m_manual, m_model.modeKnown() && !m_model.isAutoMode());
    setActiveState(m_auto, m_model.modeKnown() && m_model.isAutoMode());
    setActiveState(m_start, m_model.modeKnown() && m_model.isRunning());
    setActiveState(m_stop, m_model.modeKnown() && !m_model.isRunning());

    // Mode switch: admin only (spec §11.4).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::ModeSwitch);
        const InterlockResult i = m_model.snapshotFresh()
            ? InterlockRules::checkModeSwitch(s, online)
            : InterlockResult{false, {QStringLiteral("通讯中断或数据过期")}};
        m_manual->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
        m_auto->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Start: operator/admin + interlocks (spec §10.4).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::Start);
        const InterlockResult i = m_model.snapshotFresh()
            ? InterlockRules::checkStart(s, online)
            : InterlockResult{false, {QStringLiteral("通讯中断或数据过期")}};
        m_start->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Stop: any user, online only (spec §10.5).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::Stop);
        const InterlockResult i = m_model.snapshotFresh()
            ? InterlockRules::checkStop(s, online)
            : InterlockResult{false, {QStringLiteral("通讯中断, 命令无法送达, 请使用现场停止或实体急停")}};
        m_stop->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Reset: admin + interlocks (spec §10.2).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::Reset);
        const InterlockResult i = m_model.snapshotFresh()
            ? InterlockRules::checkReset(s, online)
            : InterlockResult{false, {QStringLiteral("通讯中断或数据过期")}};
        m_reset->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Estop set: any user, online only; offline -> "请使用实体急停" (spec §10.6).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::EstopSet);
        const InterlockResult i = m_model.snapshotFresh()
            ? InterlockRules::checkEstopSet(s, online)
            : InterlockResult{false, {QStringLiteral("通讯中断, 请使用实体急停")}};
        m_estop->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Login/logout label.
    m_login->setText(m_model.role() == Role::Anonymous ? QStringLiteral("登录")
                                                       : QStringLiteral("注销"));
}

} // namespace hlm
