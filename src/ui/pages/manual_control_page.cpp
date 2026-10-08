#include "ui/pages/manual_control_page.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/disabled_hint.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"
#include "ui/widgets/value_display.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QHideEvent>
#include <QSizePolicy>
#include <QSpinBox>

#include "domain/width_units.h"

namespace hlm {

namespace {
// Protocol addresses (0-based, matching AddressTable / ControlCoordinator).
constexpr quint16 kM42 = 42;   // 皮带常转
constexpr quint16 kM105 = 105; // 直通模式
constexpr quint16 kM106 = 106; // 手动调宽正转 (hold)
constexpr quint16 kM107 = 107; // 手动调宽反转 (hold)
constexpr quint16 kM108 = 108; // 手动皮带点动 (hold)
constexpr quint16 kM109 = 109; // 手动挡停 (latch)
constexpr quint16 kM110 = 110; // 光栅屏蔽
constexpr quint16 kM111 = 111; // 门磁屏蔽
// 测试信号 (user decision 2026-09-22): 台架测试用的脉冲信号.
constexpr quint16 kM114 = 114; // 模拟前站进板信号
constexpr quint16 kM115 = 115; // 模拟后站要板信号
constexpr quint16 kM116 = 116; // 模拟前站要板请求信号
constexpr quint16 kM117 = 117; // 模拟后站出站请求信号

// One line of the compact bullet font: the height every disabled-reason slot
// under a button keeps, whether or not it currently carries text.
constexpr int kReasonSlotHeight = 20;

// Fixes a reason slot to one line and keeps its space while it is empty
// (user decision 2026-09-24: 手动控制一栏 要求和设备操作一样 按钮大小固定).
// The labels used to be hidden outright when their gate opened, which collapsed
// the whole column and re-flowed the page by a block's worth of pixels on every
// interlock change. retainSizeWhenHidden keeps the column geometry still while
// the label stays genuinely non-visible when allowed, which is what the
// visible-reason tests require.
void makeFixedReasonSlot(QLabel *label)
{
    label->setFixedHeight(kReasonSlotHeight);
    QSizePolicy policy = label->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    label->setSizePolicy(policy);
}
} // namespace

ManualControlPage::ManualControlPage(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_pageModel(model, this)
{
    setObjectName(QStringLiteral("manualControlPage"));
    buildLayout();
    connect(&m_model, &ShellModel::stateChanged, this, &ManualControlPage::refresh);
    refresh();
}

void ManualControlPage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). Structure follows
    // 需求/PLC上位机地址及要求.txt: 手动调宽 (M106/M107)、皮带点动 (M108)、
    // 挡停 (M109)、调宽速度 (D220)、直通 (M105)、皮带常转 (M42).
    //
    // 安全屏蔽 (M110 光栅 / M111 门磁) is not surfaced here (user decision
    // 2026-09-22: 手动界面去掉安全光栅和门磁): neither the two-step confirm
    // buttons nor the 安全屏蔽生效 banner. The model/coordinator support for
    // M110/M111 is untouched, so the addresses stay writable through
    // ControlCoordinator::bypass and nothing else in the interlock chain moves.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- 手动操作区 (M106/M107/M108/M109) --------------------------------------
    auto *manualBox = new QFrame(this);
    manualBox->setObjectName(QStringLiteral("manualControlPanel"));
    manualBox->setFrameShape(QFrame::StyledPanel);
    auto *manualLayout = new QVBoxLayout(manualBox);
    manualLayout->setSpacing(8);
    auto *manualTitle = new QLabel(QStringLiteral("手动操作"), manualBox);
    manualLayout->addWidget(manualTitle);

    auto *manualRow = new QHBoxLayout();
    manualRow->setSpacing(8);
    // PLC-HMI-011 D5: the width jogs (M106/M107) are the only manual controls
    // that still require 回原点完成, so each width button carries its own
    // visible inline reason directly beneath it (spec §11.4: the reason must be
    // adjacent to the control, never tooltip-only).
    auto *widthFwdColumn = new QVBoxLayout();
    widthFwdColumn->setSpacing(4);
    m_widthFwd = new HoldButton(QStringLiteral("调宽正转"), manualBox);
    m_widthFwd->setObjectName(QStringLiteral("widthFwdButton"));
    m_widthFwd->setMinimumHeight(64);
    widthFwdColumn->addWidget(m_widthFwd);
    m_widthReason = new QLabel(manualBox);
    m_widthReason->setObjectName(QStringLiteral("widthJogReason"));
    m_widthReason->setWordWrap(true);
    makeFixedReasonSlot(m_widthReason);
    widthFwdColumn->addWidget(m_widthReason);
    manualRow->addLayout(widthFwdColumn);

    auto *widthRevColumn = new QVBoxLayout();
    widthRevColumn->setSpacing(4);
    m_widthRev = new HoldButton(QStringLiteral("调宽反转"), manualBox);
    m_widthRev->setObjectName(QStringLiteral("widthRevButton"));
    m_widthRev->setMinimumHeight(64);
    widthRevColumn->addWidget(m_widthRev);
    m_widthRevReason = new QLabel(manualBox);
    m_widthRevReason->setObjectName(QStringLiteral("widthJogReasonRev"));
    m_widthRevReason->setWordWrap(true);
    makeFixedReasonSlot(m_widthRevReason);
    widthRevColumn->addWidget(m_widthRevReason);
    manualRow->addLayout(widthRevColumn);

    // 皮带点动 has no homing precondition (PLC-HMI-011 D5) but is still gated by
    // permission/mode/estop/fault, so it carries its own visible reason beneath
    // it exactly like the width jogs (spec §11.4). Before this it was the one
    // manual control that went dead with nothing on screen or on hover to say
    // why (user decision 2026-09-22).
    auto *jogColumn = new QVBoxLayout();
    jogColumn->setSpacing(4);
    m_jog = new HoldButton(QStringLiteral("皮带点动"), manualBox);
    m_jog->setObjectName(QStringLiteral("beltJogButton"));
    m_jog->setMinimumHeight(64);
    jogColumn->addWidget(m_jog);
    m_jogReason = new QLabel(manualBox);
    m_jogReason->setObjectName(QStringLiteral("beltJogReason"));
    m_jogReason->setWordWrap(true);
    makeFixedReasonSlot(m_jogReason);
    jogColumn->addWidget(m_jogReason);
    manualRow->addLayout(jogColumn);
    m_stopGate = new PermissionButton(QStringLiteral("挡停伸出"), manualBox);
    m_stopGate->setObjectName(QStringLiteral("stopGateButton"));
    m_stopGate->setMinimumHeight(64);
    manualRow->addWidget(m_stopGate);
    manualLayout->addLayout(manualRow);

    // --- 直通 / 常转区 (M105/M42) ------------------------------------------------
    auto *bypassBox = new QFrame(this);
    bypassBox->setObjectName(QStringLiteral("bypassPanel"));
    bypassBox->setFrameShape(QFrame::StyledPanel);
    auto *bypassLayout = new QVBoxLayout(bypassBox);
    bypassLayout->setSpacing(8);
    auto *bypassTitle = new QLabel(QStringLiteral("直通 / 常转"), bypassBox);
    bypassLayout->addWidget(bypassTitle);

    auto *bypassRow = new QHBoxLayout();
    bypassRow->setSpacing(8);
    m_passthrough = new PermissionButton(QStringLiteral("直通模式"), bypassBox);
    m_passthrough->setObjectName(QStringLiteral("passthroughButton"));
    m_passthrough->setMinimumHeight(64);
    bypassRow->addWidget(m_passthrough);
    m_beltContinuous = new PermissionButton(QStringLiteral("皮带常转"), bypassBox);
    m_beltContinuous->setObjectName(QStringLiteral("beltContinuousButton"));
    m_beltContinuous->setMinimumHeight(64);
    bypassRow->addWidget(m_beltContinuous);
    bypassLayout->addLayout(bypassRow);

    // --- 测试信号 (M15 + M114-M117, user decision 2026-09-22) -------------------
    // 台架测试用: simulate the neighbouring-station handshake and the scan
    // cycle's end. Each button sends one 100 ms pulse (1 -> 100 ms -> 0) like
    // M101/M102/M103/M43. M114-M117 are absent from the current PLC program, so
    // on an unchanged PLC those pulses are inert; the PLC engineer adds the
    // rungs that consume them. M15 拍照结束 is the coil the HMI itself reads, so
    // its pulse is the bench injection that drives the whole barcode path: the
    // rising edge is detected on the snapshot feed and the result file is read.
    auto *simBox = new QFrame(this);
    simBox->setObjectName(QStringLiteral("simSignalPanel"));
    simBox->setFrameShape(QFrame::StyledPanel);
    auto *simLayout = new QVBoxLayout(simBox);
    simLayout->setSpacing(8);
    auto *simTitle = new QLabel(QStringLiteral("测试信号 (模拟前后站/拍照结束)"), simBox);
    simTitle->setObjectName(QStringLiteral("sectionTitle"));
    simLayout->addWidget(simTitle);

    auto *simRow = new QHBoxLayout();
    simRow->setSpacing(8);
    const struct {
        quint16 address;
        const char *text;
        const char *objectName;
    } simSignals[] = {
        {kM114, "模拟前站进板", "simUpstreamBoardInButton"},
        {kM115, "模拟后站要板", "simDownstreamBoardRequestButton"},
        {kM116, "模拟前站要板请求", "simUpstreamBoardRequestButton"},
        {kM117, "模拟后站出站请求", "simDownstreamExitRequestButton"},
    };
    for (const auto &signal : simSignals) {
        auto *button = new PermissionButton(QString::fromUtf8(signal.text), simBox);
        button->setObjectName(QString::fromUtf8(signal.objectName));
        button->setMinimumHeight(64);
        const quint16 address = signal.address;
        connect(button, &QPushButton::clicked, this,
                [this, address] { emit simStationPulseRequested(address); });
        simRow->addWidget(button);
        m_simSignalButtons.insert(address, button);
    }
    simLayout->addLayout(simRow);

    // --- 调宽速度写入 (D220, user decision 2026-09-22) ---------------------------
    // 调宽速度在这里可配置: the same register the users/settings page writes,
    // exposed where the operator actually jogs the width. Admin-only; the
    // write result is reported inline by setWidthSpeedWriteResult (never
    // silent, spec §11.2).
    auto *speedBox = new QFrame(this);
    speedBox->setObjectName(QStringLiteral("widthSpeedPanel"));
    speedBox->setFrameShape(QFrame::StyledPanel);
    auto *speedLayout = new QVBoxLayout(speedBox);
    speedLayout->setSpacing(8);
    auto *speedTitle = new QLabel(QStringLiteral("调宽速度 (D220)"), speedBox);
    speedTitle->setObjectName(QStringLiteral("sectionTitle"));
    speedLayout->addWidget(speedTitle);

    auto *speedRow = new QHBoxLayout();
    speedRow->setSpacing(12);
    m_widthSpeedSpin = new QSpinBox(speedBox);
    m_widthSpeedSpin->setObjectName(QStringLiteral("widthSpeedSpin"));
    m_widthSpeedSpin->setRange(1, 15); // decoded PLC clamp (PLC-HMI-005 D1)
    m_widthSpeedSpin->setSuffix(QStringLiteral(" mm/s"));
    m_widthSpeedSpin->setMinimumHeight(56);
    // The editor starts at the low end of the decoded 1-15 range; the first
    // refresh seeds it from the confirmed readback. The baseline must start at
    // the editor's own value, otherwise that first seeding looks like an
    // operator edit and the readback would never reach the editor.
    m_lastSeededWidthSpeed = m_widthSpeedSpin->value();
    speedRow->addWidget(m_widthSpeedSpin);
    m_writeWidthSpeed = new PermissionButton(QStringLiteral("写入调宽速度"), speedBox);
    m_writeWidthSpeed->setObjectName(QStringLiteral("writeWidthSpeedButton"));
    m_writeWidthSpeed->setMinimumHeight(56);
    speedRow->addWidget(m_writeWidthSpeed);
    speedRow->addStretch();
    speedLayout->addLayout(speedRow);

    m_widthSpeedResult = new QLabel(speedBox);
    m_widthSpeedResult->setObjectName(QStringLiteral("widthSpeedWriteResult"));
    m_widthSpeedResult->setWordWrap(true);
    m_widthSpeedResult->setMinimumHeight(24);
    speedLayout->addWidget(m_widthSpeedResult);
    connect(m_writeWidthSpeed, &QPushButton::clicked, this,
            &ManualControlPage::onWriteWidthSpeedClicked);

    // --- 调宽速度 (D220, 只读回读) / 当前宽度 (D130, 实时回读) + 状态行 ----------
    auto *infoRow = new QHBoxLayout();
    infoRow->setSpacing(24);
    infoRow->addWidget(addField(QStringLiteral("widthSpeed"),
                                QStringLiteral("调宽速度 (D220)")));
    // D130 当前宽度 实时回读 (user decision 2026-09-22): while jogging the
    // operator needs the live position, not only the recipe target.
    infoRow->addWidget(addField(QStringLiteral("currentWidth"),
                                QStringLiteral("当前宽度 (D130)")));
    infoRow->addStretch();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("manualStatus"));
    m_statusLabel->setMinimumHeight(48);
    infoRow->addWidget(m_statusLabel, /*stretch=*/1);
    root->addLayout(infoRow);

    root->addWidget(manualBox, /*stretch=*/1);
    root->addWidget(bypassBox, /*stretch=*/1);
    root->addWidget(simBox, /*stretch=*/1);
    root->addWidget(speedBox, /*stretch=*/1);

    // --- wiring -----------------------------------------------------------------
    // HoldButtons forward their hold state to the coordinator intent
    // (spec §10.7: 按住写 1, 松开写 0). The page never does I/O itself.
    connect(m_widthFwd, &HoldButton::holdChanged, this,
            [this](bool pressed) { emit manualHoldRequested(kM106, pressed); });
    connect(m_widthRev, &HoldButton::holdChanged, this,
            [this](bool pressed) { emit manualHoldRequested(kM107, pressed); });
    connect(m_jog, &HoldButton::holdChanged, this,
            [this](bool pressed) { emit manualHoldRequested(kM108, pressed); });
    connect(m_stopGate, &QPushButton::clicked, this,
            &ManualControlPage::onStopGateClicked);
    connect(m_passthrough, &QPushButton::clicked, this,
            [this] {
                // M105 直通: 仅管理员, 目标来自当前回读位 (spec §10.8, §11.2).
                emit bypassRequested(kM105, !m_pageModel.m105());
            });
    connect(m_beltContinuous, &QPushButton::clicked, this,
            [this] {
                // M42 皮带常转: 仅管理员, 目标来自当前回读位 (spec §10.8, §11.2).
                emit bypassRequested(kM42, !m_pageModel.m42());
            });
}

QWidget *ManualControlPage::addField(const QString &key, const QString &title)
{
    auto *titleWrap = new QWidget(this);
    titleWrap->setObjectName(QStringLiteral("valueField"));
    titleWrap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *layout = new QVBoxLayout(titleWrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *titleLabel = new QLabel(title, titleWrap);
    titleLabel->setObjectName(QStringLiteral("valueFieldTitle"));
    layout->addWidget(titleLabel);
    auto *display = new ValueDisplay(titleWrap);
    display->setMinimumHeight(48);
    display->setMaximumHeight(54);
    display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(display);
    m_displays.insert(key, display);
    return titleWrap;
}

ValueDisplay *ManualControlPage::fieldDisplay(const QString &key) const
{
    return m_displays.value(key, nullptr);
}

void ManualControlPage::onWriteWidthSpeedClicked()
{
    // The click is a request, not an effect: the value is shown as 正在写入 and
    // only the app shell's confirmed result replaces it (spec §11.2 无乐观更新).
    const quint16 value = quint16(m_widthSpeedSpin->value());
    m_widthSpeedResult->setText(
        QStringLiteral("正在写入调宽速度 %1 mm/s…").arg(value));
    m_widthSpeedResult->setStyleSheet(QStringLiteral("color: #606266;"));
    emit widthSpeedWriteRequested(value);
}

void ManualControlPage::setWidthSpeedWriteResult(bool ok, const QString &detail)
{
    if (m_widthSpeedResult == nullptr)
        return;
    // The app shell owns the message (it knows the confirmed value); the page
    // only guarantees that a terminal result is never silent.
    const QString text = !detail.isEmpty()
        ? detail
        : (ok ? QStringLiteral("调宽速度已写入")
              : QStringLiteral("调宽速度写入失败"));
    m_widthSpeedResult->setText(text);
    m_widthSpeedResult->setStyleSheet(
        ok ? QStringLiteral("color: #67c23a;") : QStringLiteral("color: #f56c6c;"));
}

QString ManualControlPage::widthSpeedResultText() const
{
    return m_widthSpeedResult ? m_widthSpeedResult->text() : QString();
}

QString ManualControlPage::statusText() const
{
    return m_statusLabel ? m_statusLabel->text() : QString();
}

void ManualControlPage::onStopGateClicked()
{
    // M109 挡停 is a latched command (spec §10.7): the click toggles the
    // target derived from the CURRENT readback, never from the button state.
    const bool target = !m_pageModel.m109();
    emit manualLatchRequested(kM109, target);
}

void ManualControlPage::hideEvent(QHideEvent *event)
{
    // Page switch (QStackedWidget hides the page) clears any armed shield
    // confirmation still held by the model (spec §10.8 二次确认, §11.1-§11.2
    // 页面切换清零意图). The page no longer renders the shield controls
    // (user decision 2026-09-22), but the model can still be armed by a caller,
    // so the reset stays here.
    m_pageModel.disarmAllShields();
    QWidget::hideEvent(event);
}

void ManualControlPage::refresh()
{
    // Full re-render from the model (spec §9: 完整快照更新, 无乐观更新).

    // D220 调宽速度: invalid/stale -> "—" (spec §9, §11.2).
    const bool speedValid = m_pageModel.widthSpeedValid();
    m_displays[QStringLiteral("widthSpeed")]->setValue(
        speedValid ? QString::number(m_pageModel.widthSpeed()) : QString(),
        QStringLiteral("mm/s"), speedValid);

    // D130 当前宽度 实时回读: raw 0.1mm -> mm (domain/width_units.h).
    const bool widthValid = m_pageModel.currentWidthValid();
    m_displays[QStringLiteral("currentWidth")]->setValue(
        widthValid ? width_units::rawToDisplay(m_pageModel.currentWidth())
                   : QString(),
        QStringLiteral("mm"), widthValid);

    // 调宽速度编辑器 (user decision 2026-09-22): seed from the confirmed
    // readback (spec §11.2 无乐观更新). The editor is re-rendered only while it
    // still shows the last value this page seeded, so an in-progress edit is
    // never clobbered by a refresh — the same rule the users/settings page
    // applies to its D220 editor.
    if (speedValid) {
        const int readback = int(m_pageModel.widthSpeed());
        if (m_widthSpeedSpin->value() == m_lastSeededWidthSpeed)
            m_widthSpeedSpin->setValue(readback);
        m_lastSeededWidthSpeed = readback;
    }
    const bool canWriteSpeed = m_pageModel.canWriteWidthSpeed();
    const QString speedReason =
        m_pageModel.widthSpeedWriteUnmetReasons().join(QStringLiteral("；"));
    m_widthSpeedSpin->setEnabled(canWriteSpeed);
    // The editor itself carries no reason text, so its hover hint mirrors the
    // write button's reason (user decision 2026-09-22).
    setUnavailableHint(m_widthSpeedSpin, canWriteSpeed, speedReason);
    m_writeWidthSpeed->setEnabledWithReason(canWriteSpeed, speedReason);

    // Manual gating: permission + interlock reasons (spec §11.4). PLC-HMI-011 D5
    // splits the manual gate per command: the width jogs (M106/M107) still
    // require homing completion (M61=1) and no homing in progress (M50=0), the
    // belt jog (M108) and the stop gate (M109) keep the common gates only.
    const bool canWidth = m_pageModel.canWidthJog();
    const QString widthReason =
        m_pageModel.widthJogUnmetReasons().join(QStringLiteral("；"));
    m_widthFwd->setEnabled(canWidth);
    m_widthRev->setEnabled(canWidth);
    // Hover hint (user decision 2026-09-22): the HoldButtons are not
    // PermissionButtons, so the hint is set here, next to the inline label.
    setUnavailableHint(m_widthFwd, canWidth, widthReason);
    setUnavailableHint(m_widthRev, canWidth, widthReason);
    // The remaining width-jog reason is visible inline text directly beneath
    // each width button, never tooltip-only (spec §11.4; the OB-7 surface
    // requirement).
    m_widthReason->setText(canWidth ? QString() : widthReason);
    m_widthReason->setVisible(!canWidth && !widthReason.isEmpty());
    m_widthRevReason->setText(canWidth ? QString() : widthReason);
    m_widthRevReason->setVisible(!canWidth && !widthReason.isEmpty());

    const bool canJog = m_pageModel.canBeltJog();
    const QString jogReason =
        m_pageModel.beltJogUnmetReasons().join(QStringLiteral("；"));
    m_jog->setEnabled(canJog);
    setUnavailableHint(m_jog, canJog, jogReason);
    m_jogReason->setText(canJog ? QString() : jogReason);
    m_jogReason->setVisible(!canJog && !jogReason.isEmpty());
    m_stopGate->setEnabledWithReason(
        m_pageModel.canStopGate(),
        m_pageModel.stopGateUnmetReasons().join(QStringLiteral("；")));

    // Bypass gating: permission + interlock reasons (spec §11.4).
    const QStringList bypassReasons = m_pageModel.bypassUnmetReasons();
    const bool canBypass = m_pageModel.canBypass();
    const QString bypassReason = bypassReasons.join(QStringLiteral("；"));
    m_passthrough->setEnabledWithReason(canBypass, bypassReason);
    m_beltContinuous->setEnabledWithReason(canBypass, bypassReason);

    // Readback state (spec §11.2: 状态来自回读位, 不是按钮状态).
    m_stopGate->setText(m_pageModel.m109() ? QStringLiteral("挡停缩回")
                                           : QStringLiteral("挡停伸出"));
    m_passthrough->setText(m_pageModel.m105() ? QStringLiteral("直通生效")
                                              : QStringLiteral("直通模式"));
    m_beltContinuous->setText(m_pageModel.m42() ? QStringLiteral("常转生效")
                                                : QStringLiteral("皮带常转"));

    // 测试信号 gating (user decision 2026-09-22): 仅管理员 + 在线, 无机器状态
    // 前置条件, 因此在自动流程运行中依然可注入. 每个按钮的禁用原因必须是可见
    // 文本 (PermissionButton 已内联渲染).
    const QStringList simReasons = m_pageModel.simSignalUnmetReasons();
    const bool canSim = m_pageModel.canSimSignal();
    const QString simReason = simReasons.join(QStringLiteral("；"));
    for (PermissionButton *button : m_simSignalButtons)
        button->setEnabledWithReason(canSim, simReason);

    // Status line.
    m_statusLabel->setText(m_pageModel.statusText());
}

} // namespace hlm
