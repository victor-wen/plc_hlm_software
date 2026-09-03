#include "ui/pages/manual_control_page.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"
#include "ui/widgets/value_display.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QHideEvent>

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
    // 挡停 (M109)、调宽速度 (D220)、直通 (M105)、皮带常转 (M42)、
    // 光栅屏蔽 (M110)、门磁屏蔽 (M111).
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- 安全屏蔽生效横幅 (spec §10.8: 生效期间顶部持续显示琥珀色横幅) --------
    m_shieldBanner = new QLabel(this);
    m_shieldBanner->setObjectName(QStringLiteral("shieldBanner"));
    m_shieldBanner->setMinimumHeight(40);
    m_shieldBanner->setAlignment(Qt::AlignCenter);
    m_shieldBanner->setStyleSheet(
        QStringLiteral("QLabel#shieldBanner { background-color: #e6a23c;"
                       " color: #3a2a00; font-size: 18px; font-weight: bold;"
                       " border-radius: 4px; }"));
    root->addWidget(m_shieldBanner);

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
    m_widthFwd = new HoldButton(QStringLiteral("调宽正转"), manualBox);
    m_widthFwd->setObjectName(QStringLiteral("widthFwdButton"));
    m_widthFwd->setMinimumHeight(64);
    manualRow->addWidget(m_widthFwd);
    m_widthRev = new HoldButton(QStringLiteral("调宽反转"), manualBox);
    m_widthRev->setObjectName(QStringLiteral("widthRevButton"));
    m_widthRev->setMinimumHeight(64);
    manualRow->addWidget(m_widthRev);
    m_jog = new HoldButton(QStringLiteral("皮带点动"), manualBox);
    m_jog->setObjectName(QStringLiteral("beltJogButton"));
    m_jog->setMinimumHeight(64);
    manualRow->addWidget(m_jog);
    m_stopGate = new PermissionButton(QStringLiteral("挡停伸出"), manualBox);
    m_stopGate->setObjectName(QStringLiteral("stopGateButton"));
    m_stopGate->setMinimumHeight(64);
    manualRow->addWidget(m_stopGate);
    manualLayout->addLayout(manualRow);

    // --- 直通 / 常转 / 安全屏蔽区 (M105/M42/M110/M111) --------------------------
    auto *bypassBox = new QFrame(this);
    bypassBox->setObjectName(QStringLiteral("bypassPanel"));
    bypassBox->setFrameShape(QFrame::StyledPanel);
    auto *bypassLayout = new QVBoxLayout(bypassBox);
    bypassLayout->setSpacing(8);
    auto *bypassTitle = new QLabel(QStringLiteral("直通 / 常转 / 安全屏蔽"), bypassBox);
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
    m_curtainShield = new PermissionButton(QStringLiteral("光栅屏蔽"), bypassBox);
    m_curtainShield->setObjectName(QStringLiteral("curtainShieldButton"));
    m_curtainShield->setMinimumHeight(64);
    bypassRow->addWidget(m_curtainShield);
    m_doorShield = new PermissionButton(QStringLiteral("门磁屏蔽"), bypassBox);
    m_doorShield->setObjectName(QStringLiteral("doorShieldButton"));
    m_doorShield->setMinimumHeight(64);
    bypassRow->addWidget(m_doorShield);
    bypassLayout->addLayout(bypassRow);

    // --- 调宽速度 (D220, 只读显示) + 状态行 --------------------------------------
    auto *infoRow = new QHBoxLayout();
    infoRow->setSpacing(24);
    infoRow->addWidget(addField(QStringLiteral("widthSpeed"),
                                QStringLiteral("调宽速度 (D220)")));
    infoRow->addStretch();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("manualStatus"));
    m_statusLabel->setMinimumHeight(48);
    infoRow->addWidget(m_statusLabel, /*stretch=*/1);
    root->addLayout(infoRow);

    root->addWidget(manualBox, /*stretch=*/1);
    root->addWidget(bypassBox, /*stretch=*/1);

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
    connect(m_curtainShield, &QPushButton::clicked, this,
            [this] { onShieldClicked(m_curtainShield, kM110); });
    connect(m_doorShield, &QPushButton::clicked, this,
            [this] { onShieldClicked(m_doorShield, kM111); });
}

ValueDisplay *ManualControlPage::addField(const QString &key, const QString &title)
{
    auto *titleWrap = new QWidget(this);
    auto *layout = new QVBoxLayout(titleWrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *titleLabel = new QLabel(title, titleWrap);
    layout->addWidget(titleLabel);
    auto *display = new ValueDisplay(titleWrap);
    display->setMinimumHeight(48);
    layout->addWidget(display);
    m_displays.insert(key, display);
    return display;
}

ValueDisplay *ManualControlPage::fieldDisplay(const QString &key) const
{
    return m_displays.value(key, nullptr);
}

QString ManualControlPage::shieldBannerText() const
{
    return m_shieldBanner ? m_shieldBanner->text() : QString();
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

void ManualControlPage::onShieldClicked(PermissionButton *button, quint16 address)
{
    // 安全屏蔽二次确认 (spec §10.8): first click arms, second dispatches.
    if (!m_pageModel.shieldArmed(address)) {
        m_pageModel.armShield(address);
        button->setText(QStringLiteral("确认屏蔽?"));
        return;
    }
    const std::optional<bool> target = m_pageModel.shieldTarget(address);
    m_pageModel.disarmShield(address);
    button->setText(QStringLiteral("光栅屏蔽"));
    if (!target.has_value())
        return;
    emit bypassRequested(address, *target);
}

void ManualControlPage::disarmShield(PermissionButton *button, quint16 address)
{
    m_pageModel.disarmShield(address);
    button->setText(QStringLiteral("光栅屏蔽"));
}

void ManualControlPage::hideEvent(QHideEvent *event)
{
    // Page switch (QStackedWidget hides the page) clears the armed shield
    // confirmation (spec §10.8 二次确认, §11.1-§11.2 页面切换清零意图).
    m_pageModel.disarmAllShields();
    m_curtainShield->setText(QStringLiteral("光栅屏蔽"));
    m_doorShield->setText(QStringLiteral("门磁屏蔽"));
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

    // 安全屏蔽生效期间持续显示琥珀色横幅 (spec §10.8).
    if (m_pageModel.shieldActive()) {
        QStringList active;
        if (m_pageModel.m110())
            active.append(QStringLiteral("光栅"));
        if (m_pageModel.m111())
            active.append(QStringLiteral("门磁"));
        m_shieldBanner->setText(
            QStringLiteral("安全屏蔽生效: %1").arg(active.join(QStringLiteral("、"))));
        m_shieldBanner->show();
    } else {
        m_shieldBanner->hide();
    }

    // Manual gating: permission + interlock reasons (spec §11.4).
    const QStringList manualReasons = m_pageModel.manualUnmetReasons();
    const bool canManual = m_pageModel.canManual();
    m_widthFwd->setEnabled(canManual);
    m_widthRev->setEnabled(canManual);
    m_jog->setEnabled(canManual);
    m_stopGate->setEnabledWithReason(canManual,
                                      manualReasons.join(QStringLiteral("；")));

    // Bypass gating: permission + interlock reasons (spec §11.4).
    const QStringList bypassReasons = m_pageModel.bypassUnmetReasons();
    const bool canBypass = m_pageModel.canBypass();
    const QString bypassReason = bypassReasons.join(QStringLiteral("；"));
    m_passthrough->setEnabledWithReason(canBypass, bypassReason);
    m_beltContinuous->setEnabledWithReason(canBypass, bypassReason);
    m_curtainShield->setEnabledWithReason(canBypass, bypassReason);
    m_doorShield->setEnabledWithReason(canBypass, bypassReason);

    // A gate change that disables the shield buttons must not leave a stale
    // armed confirmation (spec §10.8, §11.1-§11.2 门控变化清零意图). The model
    // already disarms on gate change; re-sync the button labels here.
    if (!m_curtainShield->isEnabled() && m_pageModel.shieldArmed(kM110))
        disarmShield(m_curtainShield, kM110);
    if (!m_doorShield->isEnabled() && m_pageModel.shieldArmed(kM111))
        disarmShield(m_doorShield, kM111);

    // Readback state (spec §11.2: 状态来自回读位, 不是按钮状态). The armed
    // shield confirmation label is preserved until dispatch or reset.
    m_stopGate->setText(m_pageModel.m109() ? QStringLiteral("挡停缩回")
                                           : QStringLiteral("挡停伸出"));
    m_passthrough->setText(m_pageModel.m105() ? QStringLiteral("直通生效")
                                              : QStringLiteral("直通模式"));
    m_beltContinuous->setText(m_pageModel.m42() ? QStringLiteral("常转生效")
                                                : QStringLiteral("皮带常转"));
    if (!m_pageModel.shieldArmed(kM110))
        m_curtainShield->setText(m_pageModel.m110() ? QStringLiteral("屏蔽生效")
                                                    : QStringLiteral("光栅屏蔽"));
    if (!m_pageModel.shieldArmed(kM111))
        m_doorShield->setText(m_pageModel.m111() ? QStringLiteral("屏蔽生效")
                                                 : QStringLiteral("门磁屏蔽"));

    // Status line.
    m_statusLabel->setText(m_pageModel.statusText());
}

} // namespace hlm
