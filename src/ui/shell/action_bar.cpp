#include "ui/shell/action_bar.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"
#include "application/interlock_rules.h"

#include <QVBoxLayout>
#include <QStringList>
#include <QLabel>
#include <QScrollArea>
#include <QStyle>
#include <QVariant>

namespace hlm {

namespace {
// The command-status box never changes height (user decision 2026-09-24). 40 px
// holds two lines of the 12-13 px shell font on the 683x384 compact envelope.
constexpr int kCommandStatusHeight = 40;

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
// instead of duplicating the predicate. The adapters publish evidence-based
// per-block quality (PLC-HMI-003 D6), so this gate uses the real fast-block
// quality rather than assuming it Valid.
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

    // Responsive rail (PLC-HMI-006 D1/D2): the safety strip (Stop + software
    // estop) is pinned outside the scroll area so it is always visible without
    // scrolling, and every other action keeps its inline disabled reason while
    // living inside a widget-resizable scroll area. The rail can therefore
    // shrink with the envelope instead of forcing a ~563 px column minimum.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("actionBarScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setMinimumHeight(56);
    auto *content = new QWidget(m_scroll);
    content->setObjectName(QStringLiteral("actionBarContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_scroll->setWidget(content);
    root->addWidget(m_scroll, /*stretch=*/1);

    auto *title = new QLabel(QStringLiteral("设备操作"), content);
    title->setObjectName(QStringLiteral("actionBarTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    // Persistent machine-command status (D8): visible from every page, never
    // modal, and always the latest projected state + human-readable detail.
    //
    // Fixed height (user decision 2026-09-24: 要求长度高度固定). It used to be
    // min 32 / max 96 with word wrap, so a long detail string pushed every
    // button below it up and down. The detail is still written in full as the
    // tooltip; the box itself never changes size.
    m_commandStatus = new QLabel(content);
    m_commandStatus->setObjectName(QStringLiteral("commandStatus"));
    m_commandStatus->setAlignment(Qt::AlignCenter);
    m_commandStatus->setWordWrap(true);
    m_commandStatus->setFixedHeight(kCommandStatusHeight);
    m_commandStatus->setStyleSheet(QStringLiteral(
        "QLabel#commandStatus { background-color: rgba(0, 0, 0, 0.06);"
        " border: 1px solid #b8c4d0; border-radius: 4px; padding: 4px; }"));
    layout->addWidget(m_commandStatus);

    auto make = [this, content, layout](const QString &text) {
        auto *b = new PermissionButton(text, content);
        // 56 logical px remains a large touch target while keeping every
        // safety control visible on a 768 px-high / 125%-scaled display.
        b->setMinimumHeight(56);
        layout->addWidget(b);
        return b;
    };

    auto addGroupLabel = [content, layout](const QString &text) {
        auto *label = new QLabel(text, content);
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
    // 回原点 (user decision 2026-09-21): homing is its own command. 复位 only
    // sends the M103 pulse and no longer starts homing, so the operator needs a
    // dedicated control that writes M50=1.
    m_homeStart = make(QStringLiteral("回原点"));
    m_homeStart->setObjectName(QStringLiteral("homeStartButton"));
    m_reset = make(QStringLiteral("复位"));
    m_reset->setObjectName(QStringLiteral("resetButton"));

    addGroupLabel(QStringLiteral("当前账户"));
    m_login = make(QStringLiteral("登录"));
    m_login->setObjectName(QStringLiteral("loginButton"));
    layout->addStretch();

    // Pinned safety strip: Stop and the software estop stay outside the scroll
    // area, separated from the normal actions (spec §10.6), so they remain
    // fully inside the window at every supported size without scrolling.
    m_safetyStrip = new QWidget(this);
    m_safetyStrip->setObjectName(QStringLiteral("actionBarSafetyStrip"));
    auto *stripLayout = new QVBoxLayout(m_safetyStrip);
    stripLayout->setContentsMargins(0, 0, 0, 0);
    stripLayout->setSpacing(4);

    m_stop = new PermissionButton(QStringLiteral("停止"), m_safetyStrip);
    m_stop->setObjectName(QStringLiteral("stopButton"));
    m_stop->setMinimumHeight(56);
    stripLayout->addWidget(m_stop);

    // Software estop: fixed red danger style (spec §10.6). Any user may set it
    // while online.
    m_estop = new PermissionButton(QStringLiteral("软件急停"), m_safetyStrip);
    m_estop->setObjectName(QStringLiteral("estopButton"));
    m_estop->setProperty("danger", true);
    m_estop->setMinimumHeight(80);
    // text-align keeps the visible disabled reason (PermissionButton D3) below
    // the painted title instead of overlapping it.
    m_estop->setStyleSheet(
        QStringLiteral("QPushButton#estopButton { background-color: #c42b2b;"
                       " color: white; font-weight: bold; border: 3px solid #7a1010;"
                       " border-radius: 6px; text-align: top; }"
                       "QPushButton#estopButton:disabled { background-color: #8a5555; }"));
    stripLayout->addWidget(m_estop);

    root->addWidget(m_safetyStrip);

    connect(m_manual, &QPushButton::clicked, this,
            [this] { emit modeSwitchRequested(false); });
    connect(m_auto, &QPushButton::clicked, this,
            [this] { emit modeSwitchRequested(true); });
    connect(m_start, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::Start); });
    connect(m_homeStart, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::HomeStart); });
    connect(m_stop, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::Stop); });
    connect(m_reset, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::Reset); });
    connect(m_estop, &QPushButton::clicked, this,
            [this] { emit actionRequested(Command::EstopSet); });
    connect(m_login, &QPushButton::clicked, this,
            &ActionBar::loginLogoutRequested);

    connect(&m_model, &ShellModel::stateChanged, this, &ActionBar::refresh);
    connect(&m_model, &ShellModel::operatorCommandStatusChanged, this,
            [this](const OperatorCommandStatus &) { refreshCommandStatus(); });
    refresh();
}

void ActionBar::refreshCommandStatus()
{
    if (m_commandStatus == nullptr)
        return;
    const OperatorCommandStatus status = m_model.operatorCommandStatus();
    if (status.lifecycle_state == OperatorCommandState::Idle) {
        m_commandStatus->setText(QStringLiteral("就绪"));
        m_commandStatus->setToolTip(QString());
        return;
    }
    const QString text = QStringLiteral("%1: %2")
                             .arg(toString(status.lifecycle_state),
                                  status.human_readable_detail);
    m_commandStatus->setText(text);
    // The box is a fixed height, so a long detail is elided by the layout
    // rather than shrinking the controls below it; the full text stays
    // reachable on hover.
    m_commandStatus->setToolTip(text);
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

    // 回原点: admin + interlock over the fast bits (M1/M3/M0/M14) and the home
    // block's M50. M50 is optional evidence: without it the interlock reports
    // the same data reason the other actions use. Gating it on the full
    // freshness predicate made one unrelated stale block (e.g. the slow
    // parameter poll) disable a control whose own bits are confirmed.
    {
        const PermissionResult p = PermissionPolicy::check(role, Command::HomeStart);
        const InterlockResult i = gatedCheck(
            m_model.modeKnown(),
            QStringLiteral("快速状态数据无效, 无法确认运行状态 (M3)"),
            s, online, &InterlockRules::checkHomeStart);
        m_homeStart->setEnabledWithReason(p.allowed && i.allowed, combinedReason(p, i));
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

    // Persistent machine-command status (also kept current by the dedicated
    // operatorCommandStatusChanged connection).
    refreshCommandStatus();
}

} // namespace hlm
