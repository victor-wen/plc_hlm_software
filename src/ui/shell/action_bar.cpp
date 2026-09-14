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

// Mode switch, start and reset read only the M0/M1/M2/M3/M8/M14 bits, which
// live in the fast status block (D100-D140, spec §8.2). Gating them on
// ShellModel::snapshotFresh() also required every other block to be Valid, so
// any unrelated out-of-range decoded field (e.g. an un-homed D130 of 0)
// disabled the whole bar (spec §9, §11.2). ShellModel::modeKnown() scopes the
// same connected + fastQuality()==Valid gate to the fast block; reuse it
// instead of duplicating the predicate. Note: the current adapters hard-code
// the fast block to Valid, so this currently reduces to connected().
//
// gatedCheck runs an interlock once the data it depends on is usable. Offline
// keeps the interlock's own communications reason; online-but-unusable reports
// the real data reason instead of the former catch-all "通讯中断或数据过期".
InterlockResult gatedCheck(bool dataUsable, const QString &dataReason,
                           const DeviceSnapshot &s, bool online,
                           InterlockResult (*check)(const DeviceSnapshot &, bool))
{
    if (!online)
        return check(s, online);
    if (!dataUsable)
        return InterlockResult{false, {dataReason}};
    return check(s, online);
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

    // Mode switch: admin only (spec §11.4). The interlock needs only the link
    // and M3, which lives in the fast block.
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::ModeSwitch);
        const InterlockResult i = gatedCheck(
            m_model.modeKnown(),
            QStringLiteral("快速状态数据无效, 无法确认运行状态 (M3)"),
            s, online, &InterlockRules::checkModeSwitch);
        m_manual->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
        m_auto->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Start: operator/admin + interlocks over the fast-block state bits
    // (spec §10.4).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::Start);
        const InterlockResult i = gatedCheck(
            m_model.modeKnown(),
            QStringLiteral("快速状态数据无效, 无法确认启动条件"),
            s, online, &InterlockRules::checkStart);
        m_start->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Stop: any user, online only (spec §10.5). This is a write with no
    // snapshot-field dependency, so out-of-range/stale read data must not
    // disable it.
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::Stop);
        const InterlockResult i = InterlockRules::checkStop(s, online);
        m_stop->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Reset: admin + interlocks over the fast-block M3 bit (spec §10.2).
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::Reset);
        const InterlockResult i = gatedCheck(
            m_model.modeKnown(),
            QStringLiteral("快速状态数据无效, 无法确认运行状态 (M3)"),
            s, online, &InterlockRules::checkReset);
        m_reset->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Estop set: any user, online only; offline -> "请使用实体急停" (spec §10.6).
    // Like stop, it needs no snapshot field.
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::EstopSet);
        const InterlockResult i = InterlockRules::checkEstopSet(s, online);
        m_estop->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
    }

    // Login/logout label.
    m_login->setText(m_model.role() == Role::Anonymous ? QStringLiteral("登录")
                                                       : QStringLiteral("注销"));
}

} // namespace hlm
